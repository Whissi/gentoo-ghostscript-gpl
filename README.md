# Git Workflow

## Adding a new major version

```
git checkout --orphan ghostscript-10
git rm -rf .
tar -xaf /tmp/ghostscript-10.0.tar.xz -C . --strip-components=1
find . -name '.gitignore' -delete
git add .
git commit -a -m "Import Ghostscript 10.0" --signoff
git commit --amend --date="$(stat --format='%y' /tmp/ghostscript-10.0.tar.xz)"
git tag -m "Ghostscript 10.0" -a ghostscript-10.0
```

## Create patch set

```
git checkout ghostscript-9.50
git checkout -b ghostscript-9.50-gentoo
# Make modifications
git commit -a --signoff
git format-patch ghostscript-9.50..
mkdir /tmp/patches
mv *.patch /tmp/patches

# As root
cd /tmp
chown root:root -R /tmp/patches
tar -caf ghostscript-gpl-9.50-patchset-01.tar.xz patches/
```

## Re-spin patch set

```
# After new version was imported and tagged as ghostscript-9.52,
# start with checking out latest patch set
git checkout ghostscript-9.50-gentoo
# Create a new branch for our new patch set
git checkout -b ghostscript-9.52-gentoo
git rebase --onto ghostscript-9 ghostscript-9.52
```
