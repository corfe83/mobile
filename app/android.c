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
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
		LOG_FATAL("cannot find %s", class_name);
		return NULL;
	}
	return clazz;
}

static jmethodID find_method(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
	jmethodID m = (*env)->GetMethodID(env, clazz, name, sig);
	if (m == 0) {
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
		LOG_FATAL("cannot find method %s %s", name, sig);
		return 0;
	}
	return m;
}

static jmethodID find_static_method(JNIEnv *env, jclass clazz, const char *name, const char *sig) {
	jmethodID m = (*env)->GetStaticMethodID(env, clazz, name, sig);
	if (m == 0) {
		(*env)->ExceptionDescribe(env);
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

jclass clipDataClass = NULL;
jmethodID clipDataConstructor = NULL;
jclass clipDataItemClass = NULL;
jmethodID clipDataItemConstructor = NULL;
jclass clipDescriptionClass = NULL;
jmethodID clipDescriptionConstructor = NULL;

jmethodID getPrimaryClipFunc = NULL;
jmethodID getItemAtFunc = NULL;
jmethodID getTextFunc = NULL;
jmethodID charSequencetoString = NULL;
jmethodID setPrimaryClipFunc = NULL;
unsigned char clipboardFailed = 0;

const char *clipboardLastError = "";

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

const char * getLastClipboardError() {
	return clipboardLastError;
}

const char * getClipboardString() {
	if (clipboardFailed != 0) {
		return "";
	}

	JNIEnv* env = JVMEnsureAttached();
	jobject clipData = (*env)->CallObjectMethod(env, clipboardManager, getPrimaryClipFunc);
	if (clipData == NULL) {
		(*env)->ExceptionClear(env);
		clipboardLastError = "Error getting clipboard data";
		return "";
	}

	jobject clipFirstItem = (*env)->CallObjectMethod(env, clipData, getItemAtFunc, 0);
	if (clipFirstItem == NULL) {
		(*env)->ExceptionClear(env);
		clipboardLastError = "Error getting first item of clipboard";
		return "";
	}

	jobject charSequence = (*env)->CallObjectMethod(env, clipFirstItem, getTextFunc);
	if (charSequence == NULL) {
		(*env)->ExceptionClear(env);
		clipboardLastError = "Looks like no text is copied right now";
		return "";
	}

	jstring result = (jstring)(*env)->CallObjectMethod(env, charSequence, charSequencetoString);
	if (result == NULL) {
		(*env)->ExceptionClear(env);
		clipboardLastError = "CharSequence could not be converted to string";
		return "";
	}

	return (*env)->GetStringUTFChars(env, result, 0);
}

void setClipboardString(const char * input) {
	if (clipboardFailed != 0) {
		return;
	}

	JNIEnv* env = JVMEnsureAttached();

	// Single string in array of text/plain MIME type
	jstring textToSet = (*env)->NewStringUTF(env, "Text Data");
	jclass stringClass = (*env)->FindClass(env, "java/lang/String");
	jstring mimeTypeString = (*env)->NewStringUTF(env, "text/plain");
	jobjectArray mimeTypeStringArray = (jobjectArray)(*env)->NewObjectArray(env, 1, stringClass, mimeTypeString);
	if (mimeTypeStringArray == NULL) {
		(*env)->ExceptionClear(env);
		clipboardLastError = "Failed to create mime type string array";
		return;
	}

	jobject clipDescription = (*env)->NewObject(env, clipDescriptionClass, clipDescriptionConstructor, textToSet, mimeTypeStringArray);
	if (clipDescription == NULL) {
		(*env)->ExceptionClear(env);
		clipboardLastError = "Failed to create clip description";
		return;
	}

	jstring inputString = (*env)->NewStringUTF(env, input);
	jobject clipDataItem = (*env)->NewObject(env, clipDataItemClass, clipDataItemConstructor, inputString);
	if (clipDataItem == NULL) {
		(*env)->ExceptionClear(env);
		clipboardLastError = "Failed to create clip data item";
		return;
	}

	jobject clipData = (*env)->NewObject(env, clipDataClass, clipDataConstructor, clipDescription, clipDataItem);
	if (clipData == NULL) {
		(*env)->ExceptionClear(env);
		clipboardLastError = "Failed to create clip data";
		return;
	}

	(*env)->CallVoidMethod(env, clipboardManager, setPrimaryClipFunc, clipData);
}


// Called from OnStart (CANNOT be called from OnCreate)
void setupClipboardManager(ANativeActivity *activity) {
	JNIEnv* env = activity->env;

	// If we already failed, or already have it, no need to do anything here
	if (clipboardFailed == 0 && clipboardManager != NULL) {
		clipboardLastError = "";
		return;
	}
	if (clipboardFailed) {
		return;
	}

	jobject context = activity->clazz;

	jclass contextClass = (*env)->GetObjectClass(env, context);
	if (contextClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to get context class";
		return;
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
			clipboardLastError = "failed to get superclass";
			return;
		}
	}

	jobject applicationContext = (*env)->CallObjectMethod(env, context, getApplicationContextFunc);
	if (applicationContext == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to call getApplicationContext()";
		return;
	}

	contextClass = (*env)->GetObjectClass(env, applicationContext);
	if (contextClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to get applicationcontext class";
		return;
	}

	jclass generalContextClass = (*env)->FindClass(env, "android/content/Context");
	if (context == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find context class";
		return;
	}

	jfieldID clipboardServiceField = (*env)->GetStaticFieldID(env, generalContextClass, "CLIPBOARD_SERVICE", "Ljava/lang/String;");
	if (clipboardServiceField == 0) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find clipboardServiceField";
		return;
	}

	jstring clipboardServiceName = (jstring)(*env)->GetStaticObjectField(env, generalContextClass, clipboardServiceField);
	if (clipboardServiceName == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to read clipboardServiceField";
		return;
	}

	jmethodID getSystemServiceFunc = find_method(env, contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
	if (getSystemServiceFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find getSystemService method";
		return;
	}

	jobject localClipboardManager = (*env)->CallObjectMethod(env, applicationContext, getSystemServiceFunc, clipboardServiceName);
	if (localClipboardManager == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to get clipboard service";
		return;
	}

	jclass clipboardManagerClass = (*env)->FindClass(env, "android/content/ClipboardManager");
	if (clipboardManagerClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to get class of clipboardmanager";
		return;
	}

	setPrimaryClipFunc = find_method(env, clipboardManagerClass, "setPrimaryClip", "(Landroid/content/ClipData;)V");
	if (setPrimaryClipFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find setPrimaryClip method";
		return;
	}

	getPrimaryClipFunc = find_method(env, clipboardManagerClass, "getPrimaryClip", "()Landroid/content/ClipData;");
	if (getPrimaryClipFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find getPrimaryClip method";
		return;
	}

	clipDataClass = (*env)->FindClass(env, "android/content/ClipData");
	if (clipDataClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find ClipData class";
		return;
	}
	clipDataClass = (jclass)(*env)->NewGlobalRef(env, clipDataClass);

	getItemAtFunc = find_method(env, clipDataClass, "getItemAt", "(I)Landroid/content/ClipData$Item;");
	if (getItemAtFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find getItemAt method";
		return;
	}

	clipDataItemClass = (*env)->FindClass(env, "android/content/ClipData$Item");
	if (clipDataItemClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find ClipData.Item class";
		return;
	}
	clipDataItemClass = (jclass)(*env)->NewGlobalRef(env, clipDataItemClass);

	getTextFunc = find_method(env, clipDataItemClass, "getText", "()Ljava/lang/CharSequence;");
	if (getTextFunc == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find getText method";
		return;
	}

	jclass charSequenceClass = (*env)->FindClass(env, "java/lang/CharSequence");
	if (charSequenceClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find CharSequence class";
		return;
	}

	charSequencetoString = find_method(env, charSequenceClass, "toString", "()Ljava/lang/String;");
	if (charSequencetoString == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find toString method";
		return;
	}

	// Get constructors
	clipDataItemConstructor = find_method(env, clipDataItemClass, "<init>", "(Ljava/lang/CharSequence;)V");
	if (clipDataConstructor == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find ClipDataItem constructor";
		return;
	}

	clipDataConstructor = find_method(env, clipDataClass, "<init>", "(Landroid/content/ClipDescription;Landroid/content/ClipData$Item;)V");
	if (clipDataConstructor == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find ClipData constructor";
		return;
	}

	clipDescriptionClass = (*env)->FindClass(env, "android/content/ClipDescription");
	if (clipDescriptionClass == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find ClipDescription class";
		return;
	}
	clipDescriptionClass = (jclass)(*env)->NewGlobalRef(env, clipDescriptionClass);

	clipDescriptionConstructor = find_method(env, clipDescriptionClass, "<init>", "(Ljava/lang/CharSequence;[Ljava/lang/String;)V");
	if (clipDescriptionConstructor == NULL) {
		(*env)->ExceptionClear(env);
		clipboardFailed = 1;
		clipboardLastError = "failed to find ClipDescription constructor";
		return;
	}

	clipboardManager = (*env)->NewGlobalRef(env, localClipboardManager);
	clipboardLastError = "";
	return;
}
