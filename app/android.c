// Copyright 2014 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// +build android

#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include "_cgo_export.h"

#define LOG_INFO(...) __android_log_print(ANDROID_LOG_INFO, "Go", __VA_ARGS__)
#define LOG_FATAL(...) __android_log_print(ANDROID_LOG_FATAL, "Go", __VA_ARGS__)

static jclass current_class;

static jclass find_class(JNIEnv *env, const char *class_name) {
	jclass clazz = (*env)->FindClass(env, class_name);
	if (clazz == NULL) {
		(*env)->ExceptionClear(env);
		LOG_FATAL("cannot find %s", class_name);
		return NULL;
	}
	return clazz;
}

static jmethodID find_method(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
	jmethodID m = (*env)->GetMethodID(env, clazz, name, sig);
	if (m == 0) {
		(*env)->ExceptionClear(env);
		LOG_FATAL("cannot find method %s %s", name, sig);
		return 0;
	}
	return m;
}

static jmethodID find_static_method(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
	jmethodID m = (*env)->GetStaticMethodID(env, clazz, name, sig);
	if (m == 0) {
		(*env)->ExceptionClear(env);
		LOG_FATAL("cannot find method %s %s", name, sig);
		return 0;
	}
	return m;
}

static jmethodID key_rune_method;

JavaVM* vmForClipboard;
jint JNI_OnLoad(JavaVM* vm, void* reserved) {
	JNIEnv* env;
	if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
		return -1;
	}

	vmForClipboard = vm;

	return JNI_VERSION_1_6;
}

static int main_running = 0;

// Entry point from our subclassed NativeActivity.
//
// By here, the Go runtime has been initialized (as we are running in
// -buildmode=c-shared) but the first time it is called, Go's main.main
// hasn't been called yet.
//
// The Activity may be created and destroyed multiple times throughout
// the life of a single process. Each time, onCreate is called.
void ANativeActivity_onCreate(ANativeActivity *activity, void* savedState, size_t savedStateSize) {
	if (!main_running) {
		JNIEnv* env = activity->env;

		// Note that activity->clazz is mis-named.
		current_class = (*env)->GetObjectClass(env, activity->clazz);
		current_class = (*env)->NewGlobalRef(env, current_class);
		key_rune_method = find_static_method(env, current_class, "getRune", "(III)I");

		setCurrentContext(activity->vm, (*env)->NewGlobalRef(env, activity->clazz));

		// Set TMPDIR.
		jmethodID gettmpdir = find_method(env, current_class, "getTmpdir", "()Ljava/lang/String;");
		jstring jpath = (jstring)(*env)->CallObjectMethod(env, activity->clazz, gettmpdir, NULL);
		const char* tmpdir = (*env)->GetStringUTFChars(env, jpath, NULL);
		if (setenv("TMPDIR", tmpdir, 1) != 0) {
			LOG_INFO("setenv(\"TMPDIR\", \"%s\", 1) failed: %d", tmpdir, errno);
		}
		(*env)->ReleaseStringUTFChars(env, jpath, tmpdir);

		// Call the Go main.main.
		uintptr_t mainPC = (uintptr_t)dlsym(RTLD_DEFAULT, "main.main");
		if (!mainPC) {
			LOG_FATAL("missing main.main");
		}
		callMain(mainPC);
		main_running = 1;
	}

	// These functions match the methods on Activity, described at
	// http://developer.android.com/reference/android/app/Activity.html
	//
	// Note that onNativeWindowResized is not called on resize. Avoid it.
	// https://code.google.com/p/android/issues/detail?id=180645
	activity->callbacks->onStart = onStart;
	activity->callbacks->onResume = onResume;
	activity->callbacks->onSaveInstanceState = onSaveInstanceState;
	activity->callbacks->onPause = onPause;
	activity->callbacks->onStop = onStop;
	activity->callbacks->onDestroy = onDestroy;
	activity->callbacks->onWindowFocusChanged = onWindowFocusChanged;
	activity->callbacks->onNativeWindowCreated = onNativeWindowCreated;
	activity->callbacks->onNativeWindowRedrawNeeded = onNativeWindowRedrawNeeded;
	activity->callbacks->onNativeWindowDestroyed = onNativeWindowDestroyed;
	activity->callbacks->onInputQueueCreated = onInputQueueCreated;
	activity->callbacks->onInputQueueDestroyed = onInputQueueDestroyed;
	activity->callbacks->onConfigurationChanged = onConfigurationChanged;
	activity->callbacks->onLowMemory = onLowMemory;

	onCreate(activity);
}

// TODO(crawshaw): Test configuration on more devices.
static const EGLint RGB_888[] = {
	EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
	EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	EGL_BLUE_SIZE, 8,
	EGL_GREEN_SIZE, 8,
	EGL_RED_SIZE, 8,
	EGL_DEPTH_SIZE, 16,
	EGL_CONFIG_CAVEAT, EGL_NONE,
	EGL_NONE
};

EGLDisplay display = NULL;
EGLSurface surface = NULL;

static char* initEGLDisplay() {
	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(display, 0, 0)) {
		return "EGL initialize failed";
	}
	return NULL;
}

char* createEGLSurface(ANativeWindow* window) {
	char* err;
	EGLint numConfigs, format;
	EGLConfig config;
	EGLContext context;

	if (display == 0) {
		if ((err = initEGLDisplay()) != NULL) {
			return err;
		}
	}

	if (!eglChooseConfig(display, RGB_888, &config, 1, &numConfigs)) {
		return "EGL choose RGB_888 config failed";
	}
	if (numConfigs <= 0) {
		return "EGL no config found";
	}

	eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format);
	if (ANativeWindow_setBuffersGeometry(window, 0, 0, format) != 0) {
		return "EGL set buffers geometry failed";
	}

	surface = eglCreateWindowSurface(display, config, window, NULL);
	if (surface == EGL_NO_SURFACE) {
		return "EGL create surface failed";
	}

	const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);

	if (eglMakeCurrent(display, surface, surface, context) == EGL_FALSE) {
		return "eglMakeCurrent failed";
	}
	return NULL;
}

char* destroyEGLSurface() {
	if (!eglDestroySurface(display, surface)) {
		return "EGL destroy surface failed";
	}
	return NULL;
}

int32_t getKeyRune(JNIEnv* env, AInputEvent* e) {
	return (int32_t)(*env)->CallStaticIntMethod(
		env,
		current_class,
		key_rune_method,
		AInputEvent_getDeviceId(e),
		AKeyEvent_getKeyCode(e),
		AKeyEvent_getMetaState(e)
	);
}

jobject clipboardManager = NULL;
jmethodID getPrimaryClipFunc = NULL;
jmethodID getItemAtFunc = NULL;
jmethodID getTextFunc = NULL;
jmethodID charSequencetoString = NULL;
jmethodID setPrimaryClipFunc = NULL;
jmethodID clipDataConstructor = NULL;
jmethodID clipDataItemConstructor = NULL;
jmethodID clipDescriptionConstructor = NULL;
unsigned char clipboardFailed = 0;

// 1 means success, 0 means failure
JNIEnv* JVMEnsureAttached() {
	JNIEnv* env;
	if (clipboardFailed != 0) {
		return NULL;
	}

	if ((*vmForClipboard)->GetEnv(vmForClipboard, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {
		return env;
	}

	if ((*vmForClipboard)->AttachCurrentThread(vmForClipboard, &env, NULL) == JNI_OK) {
		return env;
	}

	return NULL;
}

const char * getClipboardString() {
	JNIEnv* envForClipboardThisThread = JVMEnsureAttached();
	jobject clipData = (*envForClipboardThisThread)->CallObjectMethod(envForClipboardThisThread, clipboardManager, getPrimaryClipFunc);
	if (clipData == NULL) {
		return "Error getting clipboard data";
	}

	jobject clipFirstItem = (*envForClipboardThisThread)->CallObjectMethod(envForClipboardThisThread, clipData, getItemAtFunc, 0);
	if (clipFirstItem == NULL) {
		return "Error getting first item of clipboard";
	}

	jobject charSequence = (*envForClipboardThisThread)->CallObjectMethod(envForClipboardThisThread, clipFirstItem, getTextFunc);
	if (charSequence == NULL) {
		return "Looks like no text is copied right now";
	}

	jstring result = (jstring)(*envForClipboardThisThread)->CallObjectMethod(envForClipboardThisThread, charSequence, charSequencetoString);
	if (result == NULL) {
		return "CharSequence could not be converted to string";
	}

	return (*envForClipboardThisThread)->GetStringUTFChars(envForClipboardThisThread, result, 0);
}

void setClipboardString(const char * input) {
	JNIEnv* envForClipboardThisThread = JVMEnsureAttached();
	
}


const char * setupClipboardManager(ANativeActivity *activity) {
	JNIEnv* env = activity->env;

	// If we already failed, or already have it, no need to do anything here
	if (clipboardFailed == 0 && clipboardManager != NULL) {
		return "Already initialized";
	}
	if (clipboardFailed) {
		return "Previously failed";
	}

	jobject context = activity->clazz;

	jclass contextClass = (*env)->GetObjectClass(env, context);
	if (contextClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to get context class";
	}

	// Find context
	jmethodID getApplicationContextFunc = NULL;
	while (getApplicationContextFunc == NULL) {
		getApplicationContextFunc = find_method(env, contextClass, "getApplicationContext", "()Landroid/content/Context;");
		if (getApplicationContextFunc != NULL) {
			break;
		}

		contextClass = (*env)->GetSuperclass(env, contextClass);
		if (contextClass == NULL) {
			(*env)->ExceptionClear(env);
			clipboardFailed = 1;
			return "failed to get superclass";
		}
	}

	jobject applicationContext = (*env)->CallObjectMethod(env, context, getApplicationContextFunc);
	if (applicationContext == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to call getApplicationContext()";
	}

	contextClass = (*env)->GetObjectClass(env, applicationContext);
	if (contextClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to get applicationcontext class";
	}

	jclass generalContextClass = (*env)->FindClass(env, "android/content/Context");
	if (context == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find context class";
	}

	jfieldID clipboardServiceField = (*env)->GetStaticFieldID(env, generalContextClass, "CLIPBOARD_SERVICE", "Ljava/lang/String;");
	if (clipboardServiceField == 0) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find clipboardServiceField";
	}

	jstring clipboardServiceName = (jstring)(*env)->GetStaticObjectField(env, generalContextClass, clipboardServiceField);
	if (clipboardServiceName == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to read clipboardServiceField";
	}

	jmethodID getSystemServiceFunc = find_method(env, contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
	if (getSystemServiceFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find getSystemService method";
	}

	jobject localClipboardManager = (*env)->CallObjectMethod(env, applicationContext, getSystemServiceFunc, clipboardServiceName);
	if (localClipboardManager == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to get clipboard service";
	}

	jclass clipboardManagerClass = (*env)->FindClass(env, "android/content/ClipboardManager");
	if (clipboardManagerClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to get class of clipboardmanager";
	}

	setPrimaryClipFunc = find_method(env, clipboardManagerClass, "setPrimaryClip", "(Landroid/content/ClipData;)V");
	if (setPrimaryClipFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find setPrimaryClip method";
	}

	getPrimaryClipFunc = find_method(env, clipboardManagerClass, "getPrimaryClip", "()Landroid/content/ClipData;");
	if (getPrimaryClipFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find getPrimaryClip method";
	}

	jclass clipDataClass = (*env)->FindClass(env, "android/content/ClipData");
	if (clipDataClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find ClipData class";
	}

	getItemAtFunc = find_method(env, clipDataClass, "getItemAt", "(I)Landroid/content/ClipData$Item;");
	if (getItemAtFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find getItemAt method";
	}

	jclass clipDataItemClass = (*env)->FindClass(env, "android/content/ClipData$Item");
	if (clipDataItemClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find ClipData.Item class";
	}

	getTextFunc = find_method(env, clipDataItemClass, "getText", "()Ljava/lang/CharSequence;");
	if (getTextFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find getText method";
	}

	jclass charSequenceClass = (*env)->FindClass(env, "java/lang/CharSequence");
	if (charSequenceClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find CharSequence class";
	}

	charSequencetoString = find_method(env, charSequenceClass, "toString", "()Ljava/lang/String;");
	if (charSequencetoString == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		return "failed to find toString method";
	}

	clipboardManager = (*env)->NewGlobalRef(env, localClipboardManager);
	return "";
}
