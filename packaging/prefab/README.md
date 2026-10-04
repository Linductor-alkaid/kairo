# Prefab packaging template

This directory is a template for publishing kairo through Android Prefab/AAR.

Expected AAR layout:

```text
kairo-android-<version>.aar
└── prefab/modules/kairo/
    ├── module.json
    ├── include/kairo/...
    └── libs/
        ├── android.arm64-v8a/libkairo.so
        └── android.x86_64/libkairo.so
```

Copy this `module.json` unchanged. Do not package tests or examples into the AAR. If the
consumer uses `c++_shared`, also ship `libc++_shared.so` per ABI under `jni/<abi>/`.
See `docs/PACKAGE_ANDROID.md` for the full Gradle and packaging workflow.
