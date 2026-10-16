/* the two zomdroid globals wrapped_jni.c uses */
#pragma once
#include <jni.h>
extern JavaVM *g_zomdroid_jvm;
extern __thread JNIEnv *g_zomdroid_jni_env;
