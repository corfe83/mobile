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
jobject applicationContext = NULL;

jclass contextClass = NULL;

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

char clipboardLastError[512] = {0};

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

void copyExceptionMessage(JNIEnv* env, const char *prefix, char *cStringOutput, size_t maxSize) {
	jthrowable e = (*env)->ExceptionOccurred(env);
	(*env)->ExceptionClear(env); // clears the exception; e remains valid

	jclass clazz = (*env)->GetObjectClass(env, e);
	jmethodID getMessage = (*env)->GetMethodID(env, clazz,
											"getMessage",
											"()Ljava/lang/String;");
	jstring jStringOutput = (jstring)(*env)->CallObjectMethod(env, e, getMessage);

	const char *javaOwnedOutput = (*env)->GetStringUTFChars(env, jStringOutput, NULL);
	strncpy(cStringOutput, prefix, maxSize);
	int prefixSize = strnlen(prefix, maxSize);
	if (prefixSize < maxSize) {
		strncpy(cStringOutput+prefixSize, javaOwnedOutput, maxSize-prefixSize);
	}
	(*env)->ReleaseStringUTFChars(env, jStringOutput, javaOwnedOutput);
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
		copyExceptionMessage(env, "Error getting clipboard data", clipboardLastError, sizeof(clipboardLastError));
		return "";
	}

	jobject clipFirstItem = (*env)->CallObjectMethod(env, clipData, getItemAtFunc, 0);
	if (clipFirstItem == NULL) {
		copyExceptionMessage(env, "Error getting first item of clipboard", clipboardLastError, sizeof(clipboardLastError));
		return "";
	}

	jobject charSequence = (*env)->CallObjectMethod(env, clipFirstItem, getTextFunc);
	if (charSequence == NULL) {
		copyExceptionMessage(env, "Looks like no text is copied right now", clipboardLastError, sizeof(clipboardLastError));
		return "";
	}

	jstring result = (jstring)(*env)->CallObjectMethod(env, charSequence, charSequencetoString);
	if (result == NULL) {
		copyExceptionMessage(env, "CharSequence could not be converted to string", clipboardLastError, sizeof(clipboardLastError));
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
		copyExceptionMessage(env, "Failed to create mime type string array", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	jobject clipDescription = (*env)->NewObject(env, clipDescriptionClass, clipDescriptionConstructor, textToSet, mimeTypeStringArray);
	if (clipDescription == NULL) {
		copyExceptionMessage(env, "Failed to create clip description", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	jstring inputString = (*env)->NewStringUTF(env, input);
	jobject clipDataItem = (*env)->NewObject(env, clipDataItemClass, clipDataItemConstructor, inputString);
	if (clipDataItem == NULL) {
		copyExceptionMessage(env, "Failed to create clip data item", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	jobject clipData = (*env)->NewObject(env, clipDataClass, clipDataConstructor, clipDescription, clipDataItem);
	if (clipData == NULL) {
		copyExceptionMessage(env, "Failed to create clip data", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	(*env)->CallVoidMethod(env, clipboardManager, setPrimaryClipFunc, clipData);
}


// Called from OnStart (CANNOT be called from OnCreate)
void setupClipboardManager(ANativeActivity *activity) {
	JNIEnv* env = activity->env;

	// If we already failed, or already have it, no need to do anything here
	if (clipboardFailed == 0 && clipboardManager != NULL) {
		return;
	}
	if (clipboardFailed) {
		return;
	}

	jobject context = activity->clazz;

	contextClass = (*env)->GetObjectClass(env, context);
	if (contextClass == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to get context class", clipboardLastError, sizeof(clipboardLastError));
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
			clipboardFailed = 1;
			copyExceptionMessage(env, "failed to get superclass", clipboardLastError, sizeof(clipboardLastError));
			return;
		}
	}

	applicationContext = (*env)->CallObjectMethod(env, context, getApplicationContextFunc);
	if (applicationContext == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to call getApplicationContext()", clipboardLastError, sizeof(clipboardLastError));
		return;
	}
	applicationContext = (jclass)(*env)->NewGlobalRef(env, applicationContext);

	contextClass = (*env)->GetObjectClass(env, applicationContext);
	if (contextClass == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to get applicationcontext class", clipboardLastError, sizeof(clipboardLastError));
		return;
	}
	contextClass = (jclass)(*env)->NewGlobalRef(env, contextClass);

	jclass generalContextClass = (*env)->FindClass(env, "android/content/Context");
	if (context == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find context class", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	jfieldID clipboardServiceField = (*env)->GetStaticFieldID(env, generalContextClass, "CLIPBOARD_SERVICE", "Ljava/lang/String;");
	if (clipboardServiceField == 0) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find clipboardServiceField", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	jstring clipboardServiceName = (jstring)(*env)->GetStaticObjectField(env, generalContextClass, clipboardServiceField);
	if (clipboardServiceName == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to read clipboardServiceField", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	jmethodID getSystemServiceFunc = find_method(env, contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
	if (getSystemServiceFunc == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find getSystemService method", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	jobject localClipboardManager = (*env)->CallObjectMethod(env, applicationContext, getSystemServiceFunc, clipboardServiceName);
	if (localClipboardManager == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to get clipboard service", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	jclass clipboardManagerClass = (*env)->FindClass(env, "android/content/ClipboardManager");
	if (clipboardManagerClass == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to get class of clipboardmanager", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	setPrimaryClipFunc = find_method(env, clipboardManagerClass, "setPrimaryClip", "(Landroid/content/ClipData;)V");
	if (setPrimaryClipFunc == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find setPrimaryClip method", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	getPrimaryClipFunc = find_method(env, clipboardManagerClass, "getPrimaryClip", "()Landroid/content/ClipData;");
	if (getPrimaryClipFunc == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find getPrimaryClip method", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	clipDataClass = (*env)->FindClass(env, "android/content/ClipData");
	if (clipDataClass == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find ClipData class", clipboardLastError, sizeof(clipboardLastError));
		return;
	}
	clipDataClass = (jclass)(*env)->NewGlobalRef(env, clipDataClass);

	getItemAtFunc = find_method(env, clipDataClass, "getItemAt", "(I)Landroid/content/ClipData$Item;");
	if (getItemAtFunc == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find getItemAt method", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	clipDataItemClass = (*env)->FindClass(env, "android/content/ClipData$Item");
	if (clipDataItemClass == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find ClipData.Item class", clipboardLastError, sizeof(clipboardLastError));
		return;
	}
	clipDataItemClass = (jclass)(*env)->NewGlobalRef(env, clipDataItemClass);

	getTextFunc = find_method(env, clipDataItemClass, "getText", "()Ljava/lang/CharSequence;");
	if (getTextFunc == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find getText method", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	jclass charSequenceClass = (*env)->FindClass(env, "java/lang/CharSequence");
	if (charSequenceClass == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find CharSequence class", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	charSequencetoString = find_method(env, charSequenceClass, "toString", "()Ljava/lang/String;");
	if (charSequencetoString == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find toString method", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	// Get constructors
	clipDataItemConstructor = find_method(env, clipDataItemClass, "<init>", "(Ljava/lang/CharSequence;)V");
	if (clipDataItemConstructor == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find ClipDataItem constructor", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	clipDataConstructor = find_method(env, clipDataClass, "<init>", "(Landroid/content/ClipDescription;Landroid/content/ClipData$Item;)V");
	if (clipDataConstructor == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find ClipData constructor", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	clipDescriptionClass = (*env)->FindClass(env, "android/content/ClipDescription");
	if (clipDescriptionClass == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find ClipDescription class", clipboardLastError, sizeof(clipboardLastError));
		return;
	}
	clipDescriptionClass = (jclass)(*env)->NewGlobalRef(env, clipDescriptionClass);

	clipDescriptionConstructor = find_method(env, clipDescriptionClass, "<init>", "(Ljava/lang/CharSequence;[Ljava/lang/String;)V");
	if (clipDescriptionConstructor == NULL) {
		clipboardFailed = 1;
		copyExceptionMessage(env, "failed to find ClipDescription constructor", clipboardLastError, sizeof(clipboardLastError));
		return;
	}

	clipboardManager = (*env)->NewGlobalRef(env, localClipboardManager);
	clipboardLastError[0] = '\0';

	return;
}

jclass intentClass = NULL;
jmethodID intentConstructor = NULL;

jclass uriClass = NULL;
jmethodID uriParseFunc = NULL;

jmethodID startActivityFunc = NULL;

jstring actionViewString = NULL;

unsigned char browserFailed = 0;
unsigned char browserInitCompleted = 0;
char browserLastError[512] = {0};
void setupBrowser(ANativeActivity *activity) {
	JNIEnv* env = activity->env;

	// If we already failed, or already have it, no need to do anything here
	if (browserFailed == 0 && browserInitCompleted != 0) {
		return;
	}
	if (browserFailed) {
		return;
	}

	intentClass = (*env)->FindClass(env, "android/content/Intent");
	if (intentClass == NULL) {
		browserFailed = 1;
		copyExceptionMessage(env, "failed to find Intent class", browserLastError, sizeof(browserLastError));
		return;
	}
	intentClass = (jclass)(*env)->NewGlobalRef(env, intentClass);

	intentConstructor = find_method(env, intentClass, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
	if (intentConstructor == NULL) {
		browserFailed = 1;
		copyExceptionMessage(env, "failed to find Intent constructor", browserLastError, sizeof(browserLastError));
		return;
	}

	uriClass = (*env)->FindClass(env, "android/net/Uri");
	if (uriClass == NULL) {
		browserFailed = 1;
		copyExceptionMessage(env, "failed to find Uri class", browserLastError, sizeof(browserLastError));
		return;
	}
	uriClass = (jclass)(*env)->NewGlobalRef(env, uriClass);

	uriParseFunc = find_static_method(env, uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
	if (uriParseFunc == NULL) {
		browserFailed = 1;
		copyExceptionMessage(env, "failed to find static method Uri.parse", browserLastError, sizeof(browserLastError));
		return;
	}

	jfieldID actionViewField = (*env)->GetStaticFieldID(env, intentClass, "ACTION_VIEW", "Ljava/lang/String;");
	if (actionViewField == 0) {
		browserFailed = 1;
		copyExceptionMessage(env, "failed to find Intent.ACTION_VIEW", browserLastError, sizeof(browserLastError));
		return;
	}

	actionViewString = (jstring)(*env)->GetStaticObjectField(env, intentClass, actionViewField);
	if (actionViewString == NULL) {
		browserFailed = 1;
		copyExceptionMessage(env, "failed to read Intent.ACTION_VIEW", browserLastError, sizeof(browserLastError));
		return;
	}
	actionViewString = (jstring)(*env)->NewGlobalRef(env, actionViewString);

	startActivityFunc = find_method(env, contextClass, "startActivity", "(Landroid/content/Intent;)V");
	if (intentConstructor == NULL) {
		browserFailed = 1;
		copyExceptionMessage(env, "failed to find startActivity function", browserLastError, sizeof(browserLastError));
		return;
	}

	browserInitCompleted = 1;
}

void openUrl(const char * url) {
	if (browserFailed != 0) {
		return;
	}

	JNIEnv* env = JVMEnsureAttached();

	jstring urlstring = (*env)->NewStringUTF(env, url);
	if (urlstring == NULL) {
		copyExceptionMessage(env, "Failed to create jstring for url", browserLastError, sizeof(browserLastError));
		return;
	}

	jobject uri = (jstring)(*env)->CallStaticObjectMethod(env, uriClass, uriParseFunc, urlstring);
	if (uri == NULL) {
		copyExceptionMessage(env, "Uri.parse call failed", browserLastError, sizeof(browserLastError));
		return;
	}

	jobject intent = (*env)->NewObject(env, intentClass, intentConstructor, actionViewString, uri);
	if (intent == NULL) {
		copyExceptionMessage(env, "Failed to create intent", browserLastError, sizeof(browserLastError));
		return;
	}

	(*env)->CallVoidMethod(env, applicationContext, startActivityFunc, intent);
	if ((*env)->ExceptionOccurred(env) != NULL) {
		copyExceptionMessage(env, "Failed to start activity:", browserLastError, sizeof(browserLastError));
		return;
	}
}

const char * getLastBrowserError() {
	return browserLastError;
}
