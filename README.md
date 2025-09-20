## Fixed drivers for G502 X

This repo contains
 - a branch of the linux kernel, with patches to get the G502 X + Lightspeed receiver working with the `hid-logitech-dj` and `hid-logitech-hidpp` modules.
 - some machinery to also build said patched modules with DKMS, and a Debian package of this.

## Usage
 - Download and install `new-hidpp-dkms_*.deb` from the latest Github release.

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
