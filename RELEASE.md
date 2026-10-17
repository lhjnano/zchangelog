# Release process

Versions follow semver (MAJOR.MINOR.PATCH). The version lives in
configure.ac (AC_INIT); debian/changelog and packaging/rpm/
zchangelog.spec must be bumped in the same change.

To cut a release:

1. Bump the version in configure.ac, debian/changelog and
   packaging/rpm/zchangelog.spec in one commit.
2. Tag the commit: git tag v0.1.1 && git push origin v0.1.1
3. The release workflow verifies the tag matches the configure.ac
   version, builds the source tarball (make dist) and publishes a
   GitHub release with it. Packaging jobs attach deb and rpm
   artifacts.

The workflow fails if a tag does not match the configure.ac version.
