// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ChaCha stream cipher (P10 accelerated)
 *
 * Copyright 2023- IBM Corp. All rights reserved.
 */

#include <crypto/chacha.h>
#include <crypto/internal/simd.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpufeature.h>
#include <linux/sizes.h>
#include <asm/simd.h>
#include <asm/switch_to.h>

asmlinkage void chacha_p10le_8x(u32 *state, u8 *dst, const u8 *src,
				unsigned int len, int nrounds);

static __ro_after_init DEFINE_STATIC_KEY_FALSE(have_p10);

static void vsx_begin(void)
{
	preempt_disable();
	enable_kernel_vsx();
}

static void vsx_end(void)
{
	disable_kernel_vsx();
	preempt_enable();
}

static void chacha_p10_do_8x(u32 *state, u8 *dst, const u8 *src,
			     unsigned int bytes, int nrounds)
{
	unsigned int l = bytes & ~0x0FF;

	if (l > 0) {
		chacha_p10le_8x(state, dst, src, l, nrounds);
		bytes -= l;
		src += l;
		dst += l;
		state[12] += l / CHACHA_BLOCK_SIZE;
	}

	if (bytes > 0)
		chacha_crypt_generic(state, dst, src, bytes, nrounds);
}

void hchacha_block_arch(const u32 *state, u32 *stream, int nrounds)
{
	hchacha_block_generic(state, stream, nrounds);
}
EXPORT_SYMBOL(hchacha_block_arch);

void chacha_crypt_arch(u32 *state, u8 *dst, const u8 *src, unsigned int bytes,
		       int nrounds)
{
	if (!static_branch_likely(&have_p10) || bytes <= CHACHA_BLOCK_SIZE ||
	    !crypto_simd_usable())
		return chacha_crypt_generic(state, dst, src, bytes, nrounds);

	do {
		unsigned int todo = min_t(unsigned int, bytes, SZ_4K);

		vsx_begin();
		chacha_p10_do_8x(state, dst, src, todo, nrounds);
		vsx_end();

		bytes -= todo;
		src += todo;
		dst += todo;
	} while (bytes);
}
EXPORT_SYMBOL(chacha_crypt_arch);

bool chacha_is_arch_optimized(void)
{
	return static_key_enabled(&have_p10);
}
EXPORT_SYMBOL(chacha_is_arch_optimized);

static int __init chacha_p10_init(void)
{
	if (cpu_has_feature(CPU_FTR_ARCH_31))
		static_branch_enable(&have_p10);
	return 0;
}
arch_initcall(chacha_p10_init);

MODULE_DESCRIPTION("ChaCha stream cipher (P10 accelerated)");
MODULE_AUTHOR("Danny Tsen <dtsen@linux.ibm.com>");
MODULE_LICENSE("GPL v2");
