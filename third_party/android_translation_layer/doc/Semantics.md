### what is this, again?

When this project was first conceptualized, it was thought of as a translation layer - something
akin to Wine. However, after becoming more familiar with the Android platform, it seems that
there is a more interesting way to frame this.

If you read through https://source.android.com/docs/compatibility/cdd, even the oldest linked
https://source.android.com/static/docs/compatibility/1.6/android-1.6-cdd.pdf, you may be intrigued
by the following:

>... For this reason, the Android Open
Source Project [Resources, 4] is both the reference and preferred implementation of Android. ...

After reading through a reasonably recent version of this document, it would seem that a Linux
distribution which uses this project to run android applications, and creatively interprets
some of the requirements, could be shipped on a device and that device could be branded as
an android device (provided that this project ever matures to a point where it can run
the full compatibility test suite)

While this is mostly fully academic, it suggests that this project could be seen as a work-in-progress
core part of an alternative implementation of Android, or possibly as a reframing of the android
platform as something akin to the Qt platform - a framework for applications to use, which is
a pseudo-platform on top of a different platform (in Qt's case, this ensures easier portability
of the resulting apps; in our case, the only platform we can realistically support is Linux,
since most apps use native libraries. BSDs with Linux compatiblity layers should also work fwiw
(THIS WAS NOT TESTED), but it's debatable whether that counts.) Funnily enough, bionic libc seems
to be decently shimmable to either glibc or musl, so that gives us binary compatibility between
glibc and musl systems.
