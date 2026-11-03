# MarvinCapture: R8 rules on top of proguard-android-optimize.txt, which already keeps the
# names of classes with native methods and of those methods (the JNI library binds them by
# their Java_jonascz_marvincapture_Core_* symbol names). Nothing here uses reflection.
-keepclasseswithmembernames,includedescriptorclasses class jonascz.marvincapture.Core {
    native <methods>;
}
