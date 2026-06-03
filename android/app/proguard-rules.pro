# Keep JNI entry points reachable from native code.
-keepclasseswithmembernames class com.nordstjernen.browser.NativeBrowser {
    native <methods>;
}
