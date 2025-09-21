## Fixed drivers for G502 X

This repo contains
 - a branch of the linux kernel, with patches to get the G502 X + Lightspeed receiver working with the `hid-logitech-dj` and `hid-logitech-hidpp` modules.
 - some machinery to also build said patched modules with DKMS, and a Debian package of this.

## Usage
 - Download and install `new-hidpp-dkms_*.deb` from the latest Github release.

## New Branch Setup


 - The `upstream` branch is the kernel master.
   - To update:
      `git checkout upstream`
      `git fetch torvalds`
      `git pull torvalds master:upstream`

 - Each `patch` branch is a rebase of the patches on a particular kernel version. `patch-current`
   is the current iteration, but this gets rebased each time.
    - To update:
       (find last commit before patches)
       `git checkout patch-current`
       `git rebase $(mainline-commit-pre-patches) --onto $(new-commit-or-tag)`, e.g. to rebase on 6.16:
       `git rebase $(mainline-commit-pre-patches) --onto v6.16`
       `git checkout -b patch-v3` (this will increment - next time patch-v4, etc.)


 - `main` is development and releases. Commits to packaging system go directly on main.
   The patches are pulled in by merging the latest patch branch and blatting existing files.
   - To update:
     `git revert (patch commits)`
     `git rebase -i (HEAD^^^^^)` (squash the patch revert)
     `git merge -X subtree=upstream/ v6.16` (new kernel, blat everything)
     `git merge -X subtree=upstream/ patch-v3` (or v4, etc.) (re-apply patches)

## Branch setup

```
 - git commit --allow-empty -m 'init'
 - git checkout -B main

 - git remote add torvalds
 - git fetch torvalds
 - git checkout ~torvalds/master~ v6.12
 - git checkout -B upstream

 - git checkout main
 # prepare for subtree merge - https://www.atlassian.com/git/tutorials/git-subtree "Do without subtree?"
 - git merge -s ours --no-commit --allow-unrelated-histories upstream
 # initialize subtree
 - git read-tree --prefix=upstream/ -u upstream
 - git commit

 # now, we can pull in upstream updates, e.g.
 - git merge -X subtree=upstream/ v6.313

 # applying HID patch
 - git checkout e400071a805d6229223a98899e9da8c6233704a1 # found by git log -p drivers/hid/hid-logitech-hidpp.c
 - git am -i FILE.patch
 - git checkout -B patch-orig
 - git checkout -B patch-upstream
 - git rebase e400071a805d6229223a98899e9da8c6233704a1 --onto upstream
   (fix conflicts)

 # now apply patch-upstream in our dkms module
 - git checkout min
 - git merge -X subtree=upstream/ patch-upstream
```

## STASHES:
 - keyboard: try and get the broken keyboard working


## Versioning

Deb package versions are maintained by `git tag -a deb-v1.0.0` -m 'v1.0.0'
