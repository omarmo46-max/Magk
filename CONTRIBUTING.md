# Contributing to Magk

Thanks for helping! A few ground rules keep the project easy to work on.

## Getting set up

See **Building from source** in the [README](README.md). The quickest route is to push
your branch and let GitHub Actions build it; locally you need JDK 17, the Android SDK
(API 36), the NDK (r27c is what CI uses), Go (version pinned in `deps/rclone-mobile/go.mod`)
and `gomobile`.

## Making a change

1. Open an issue first for anything bigger than a small fix, so we can agree on the approach.
2. Branch from `main`; keep one logical change per pull request.
3. Follow the existing Kotlin style (see `.editorconfig`); keep user-visible text in
   `res/values*/strings.xml`, never hard-coded.
4. Add or update tests under `android/app/src/test/` when behaviour changes.
5. If you touch `deps/proot-patches/`, run `deps/proot-patches/tests/run_tests.sh`.
6. Make sure the **Android build** workflow is green before asking for review.

## Naming

The product, its tools and its on-disk paths are all `Magk` / `magk-*` / `/var/magk`.
Please do not introduce other product names in code, strings or paths.

## Commit messages

Short imperative subject (`Fix backup picker filter`), then a body explaining *why* when it
is not obvious.

## Licensing

By contributing you agree that your work is licensed under the GNU GPL v3 (see [LICENSE](LICENSE)).
