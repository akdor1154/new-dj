// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"
#include "alloc_background.h"
#include "backpointers.h"
#include "btree_gc.h"
#include "btree_node_scan.h"
#include "disk_accounting.h"
#include "ec.h"
#include "fsck.h"
#include "inode.h"
#include "journal.h"
#include "lru.h"
#include "logged_ops.h"
#include "rebalance.h"
#include "recovery.h"
#include "recovery_passes.h"
#include "snapshot.h"
#include "subvolume.h"
#include "super.h"
#include "super-io.h"

const char * const bch2_recovery_passes[] = {
#define x(_fn, ...)	#_fn,
	BCH_RECOVERY_PASSES()
#undef x
	NULL
};

/* Fake recovery pass, so that scan_for_btree_nodes isn't 0: */
static int bch2_recovery_pass_empty(struct bch_fs *c)
{
	return 0;
}

static int bch2_set_may_go_rw(struct bch_fs *c)
{
	struct journal_keys *keys = &c->journal_keys;

	/*
	 * After we go RW, the journal keys buffer can't be modified (except for
	 * setting journal_key->overwritten: it will be accessed by multiple
	 * threads
	 */
	move_gap(keys, keys->nr);

	set_bit(BCH_FS_may_go_rw, &c->flags);

	if (keys->nr ||
	    !c->opts.read_only ||
	    !c->sb.clean ||
	    c->opts.recovery_passes ||
	    (c->opts.fsck && !(c->sb.features & BIT_ULL(BCH_FEATURE_no_alloc_info)))) {
		if (c->sb.features & BIT_ULL(BCH_FEATURE_no_alloc_info)) {
			bch_info(c, "mounting a filesystem with no alloc info read-write; will recreate");
			bch2_reconstruct_alloc(c);
		}

		return bch2_fs_read_write_early(c);
	}
	return 0;
}

/*
 * Make sure root inode is readable while we're still in recovery and can rewind
 * for repair:
 */
static int bch2_lookup_root_inode(struct bch_fs *c)
{
	subvol_inum inum = BCACHEFS_ROOT_SUBVOL_INUM;
	struct bch_inode_unpacked inode_u;
	struct bch_subvolume subvol;

	return bch2_trans_do(c,
		bch2_subvolume_get(trans, inum.subvol, true, &subvol) ?:
		bch2_inode_find_by_inum_trans(trans, inum, &inode_u));
}

struct recovery_pass_fn {
	int		(*fn)(struct bch_fs *);
	unsigned	when;
};

static struct recovery_pass_fn recovery_pass_fns[] = {
#define x(_fn, _id, _when)	{ .fn = bch2_##_fn, .when = _when },
	BCH_RECOVERY_PASSES()
#undef x
};

static const u8 passes_to_stable_map[] = {
#define x(n, id, ...)	[BCH_RECOVERY_PASS_##n] = BCH_RECOVERY_PASS_STABLE_##n,
	BCH_RECOVERY_PASSES()
#undef x
};

static enum bch_recovery_pass_stable bch2_recovery_pass_to_stable(enum bch_recovery_pass pass)
{
	return passes_to_stable_map[pass];
}

u64 bch2_recovery_passes_to_stable(u64 v)
{
	u64 ret = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(passes_to_stable_map); i++)
		if (v & BIT_ULL(i))
			ret |= BIT_ULL(passes_to_stable_map[i]);
	return ret;
}

u64 bch2_recovery_passes_from_stable(u64 v)
{
	static const u8 map[] = {
#define x(n, id, ...)	[BCH_RECOVERY_PASS_STABLE_##n] = BCH_RECOVERY_PASS_##n,
	BCH_RECOVERY_PASSES()
#undef x
	};

	u64 ret = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(map); i++)
		if (v & BIT_ULL(i))
			ret |= BIT_ULL(map[i]);
	return ret;
}

/*
 * For when we need to rewind recovery passes and run a pass we skipped:
 */
static int __bch2_run_explicit_recovery_pass(struct printbuf *out,
					     struct bch_fs *c,
					     enum bch_recovery_pass pass)
{
	if (c->curr_recovery_pass == ARRAY_SIZE(recovery_pass_fns))
		return -BCH_ERR_not_in_recovery;

	if (c->recovery_passes_complete & BIT_ULL(pass))
		return 0;

	bool print = !(c->opts.recovery_passes & BIT_ULL(pass));

	if (pass < BCH_RECOVERY_PASS_set_may_go_rw &&
	    c->curr_recovery_pass >= BCH_RECOVERY_PASS_set_may_go_rw) {
		if (print)
			prt_printf(out, "need recovery pass %s (%u), but already rw",
				   bch2_recovery_passes[pass], pass);
		return -BCH_ERR_cannot_rewind_recovery;
	}

	if (print)
		prt_printf(out, "running explicit recovery pass %s (%u), currently at %s (%u)",
			   bch2_recovery_passes[pass], pass,
			   bch2_recovery_passes[c->curr_recovery_pass], c->curr_recovery_pass);

	c->opts.recovery_passes |= BIT_ULL(pass);

	if (c->curr_recovery_pass > pass) {
		c->next_recovery_pass = pass;
		c->recovery_passes_complete &= (1ULL << pass) >> 1;
		return -BCH_ERR_restart_recovery;
	} else {
		return 0;
	}
}

int bch2_run_explicit_recovery_pass_printbuf(struct bch_fs *c,
				    struct printbuf *out,
				    enum bch_recovery_pass pass)
{
	bch2_printbuf_make_room(out, 1024);
	out->atomic++;

	unsigned long flags;
	spin_lock_irqsave(&c->recovery_pass_lock, flags);
	int ret = __bch2_run_explicit_recovery_pass(out, c, pass);
	spin_unlock_irqrestore(&c->recovery_pass_lock, flags);

	--out->atomic;
	return ret;
}

int bch2_run_explicit_recovery_pass(struct bch_fs *c,
				    enum bch_recovery_pass pass)
{
	struct printbuf buf = PRINTBUF;
	bch2_log_msg_start(c, &buf);
	unsigned len = buf.pos;

	int ret = bch2_run_explicit_recovery_pass_printbuf(c, &buf, pass);

	if (len != buf.pos)
		bch2_print_str(c, KERN_NOTICE, buf.buf);
	printbuf_exit(&buf);
	return ret;
}

int bch2_run_explicit_recovery_pass_persistent_locked(struct bch_fs *c,
					       enum bch_recovery_pass pass)
{
	lockdep_assert_held(&c->sb_lock);

	struct bch_sb_field_ext *ext = bch2_sb_field_get(c->disk_sb.sb, ext);
	__set_bit_le64(bch2_recovery_pass_to_stable(pass), ext->recovery_passes_required);

	return bch2_run_explicit_recovery_pass(c, pass);
}

int bch2_run_explicit_recovery_pass_persistent(struct bch_fs *c,
					       enum bch_recovery_pass pass)
{
	enum bch_recovery_pass_stable s = bch2_recovery_pass_to_stable(pass);

	mutex_lock(&c->sb_lock);
	struct bch_sb_field_ext *ext = bch2_sb_field_get(c->disk_sb.sb, ext);

	if (!test_bit_le64(s, ext->recovery_passes_required)) {
		__set_bit_le64(s, ext->recovery_passes_required);
		bch2_write_super(c);
	}
	mutex_unlock(&c->sb_lock);

	return bch2_run_explicit_recovery_pass(c, pass);
}

static void bch2_clear_recovery_pass_required(struct bch_fs *c,
					      enum bch_recovery_pass pass)
{
	enum bch_recovery_pass_stable s = bch2_recovery_pass_to_stable(pass);

	mutex_lock(&c->sb_lock);
	struct bch_sb_field_ext *ext = bch2_sb_field_get(c->disk_sb.sb, ext);

	if (test_bit_le64(s, ext->recovery_passes_required)) {
		__clear_bit_le64(s, ext->recovery_passes_required);
		bch2_write_super(c);
	}
	mutex_unlock(&c->sb_lock);
}

u64 bch2_fsck_recovery_passes(void)
{
	u64 ret = 0;

	for (unsigned i = 0; i < ARRAY_SIZE(recovery_pass_fns); i++)
		if (recovery_pass_fns[i].when & PASS_FSCK)
			ret |= BIT_ULL(i);
	return ret;
}

static bool should_run_recovery_pass(struct bch_fs *c, enum bch_recovery_pass pass)
{
	struct recovery_pass_fn *p = recovery_pass_fns + pass;

	if ((p->when & PASS_ALLOC) && (c->sb.features & BIT_ULL(BCH_FEATURE_no_alloc_info)))
		return false;
	if (c->opts.recovery_passes_exclude & BIT_ULL(pass))
		return false;
	if (c->opts.recovery_passes & BIT_ULL(pass))
		return true;
	if ((p->when & PASS_FSCK) && c->opts.fsck)
		return true;
	if ((p->when & PASS_UNCLEAN) && !c->sb.clean)
		return true;
	if (p->when & PASS_ALWAYS)
		return true;
	return false;
}

static int bch2_run_recovery_pass(struct bch_fs *c, enum bch_recovery_pass pass)
{
	struct recovery_pass_fn *p = recovery_pass_fns + pass;
	int ret;

	if (!(p->when & PASS_SILENT))
		bch2_print(c, KERN_INFO bch2_log_msg(c, "%s..."),
			   bch2_recovery_passes[pass]);
	ret = p->fn(c);
	if (ret)
		return ret;
	if (!(p->when & PASS_SILENT))
		bch2_print(c, KERN_CONT " done\n");

	return 0;
}

int bch2_run_online_recovery_passes(struct bch_fs *c)
{
	for (unsigned i = 0; i < ARRAY_SIZE(recovery_pass_fns); i++) {
		struct recovery_pass_fn *p = recovery_pass_fns + i;

		if (!(p->when & PASS_ONLINE))
			continue;

		int ret = bch2_run_recovery_pass(c, i);
		if (bch2_err_matches(ret, BCH_ERR_restart_recovery)) {
			i = c->curr_recovery_pass;
			continue;
		}
		if (ret)
			return ret;
	}

	return 0;
}

int bch2_run_recovery_passes(struct bch_fs *c)
{
	int ret = 0;

	/*
	 * We can't allow set_may_go_rw to be excluded; that would cause us to
	 * use the journal replay keys for updates where it's not expected.
	 */
	c->opts.recovery_passes_exclude &= ~BCH_RECOVERY_PASS_set_may_go_rw;

	while (c->curr_recovery_pass < ARRAY_SIZE(recovery_pass_fns) && !ret) {
		c->next_recovery_pass = c->curr_recovery_pass + 1;

		spin_lock_irq(&c->recovery_pass_lock);
		unsigned pass = c->curr_recovery_pass;

		if (c->opts.recovery_pass_last &&
		    c->curr_recovery_pass > c->opts.recovery_pass_last) {
			spin_unlock_irq(&c->recovery_pass_lock);
			break;
		}

		if (!should_run_recovery_pass(c, pass)) {
			c->curr_recovery_pass++;
			c->recovery_pass_done = max(c->recovery_pass_done, pass);
			spin_unlock_irq(&c->recovery_pass_lock);
			continue;
		}
		spin_unlock_irq(&c->recovery_pass_lock);

		ret =   bch2_run_recovery_pass(c, pass) ?:
			bch2_journal_flush(&c->journal);

		if (!ret && !test_bit(BCH_FS_error, &c->flags))
			bch2_clear_recovery_pass_required(c, pass);

		spin_lock_irq(&c->recovery_pass_lock);
		if (c->next_recovery_pass < c->curr_recovery_pass) {
			/*
			 * bch2_run_explicit_recovery_pass() was called: we
			 * can't always catch -BCH_ERR_restart_recovery because
			 * it may have been called from another thread (btree
			 * node read completion)
			 */
			ret = 0;
			c->recovery_passes_complete &= ~(~0ULL << c->curr_recovery_pass);
		} else {
			c->recovery_passes_complete |= BIT_ULL(pass);
			c->recovery_pass_done = max(c->recovery_pass_done, pass);
		}
		c->curr_recovery_pass = c->next_recovery_pass;
		spin_unlock_irq(&c->recovery_pass_lock);
	}

	return ret;
}
