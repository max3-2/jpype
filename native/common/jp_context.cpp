/*****************************************************************************
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

		http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   See NOTICE file for details.
 *****************************************************************************/
#include "jpype.h"
#include "pyjp.h"
#include "jp_typemanager.h"
#include "jp_boxedtype.h"
#include "jp_stringtype.h"
#include "jp_classloader.h"
#include "jp_voidtype.h"
#include "jp_booleantype.h"
#include "jp_bytetype.h"
#include "jp_chartype.h"
#include "jp_shorttype.h"
#include "jp_inttype.h"
#include "jp_longtype.h"
#include "jp_floattype.h"
#include "jp_doubletype.h"
#include "jp_proxy.h"
#include "jp_platform.h"
#include "jp_gc.h"

JPResource::~JPResource()
{
}


#define USE_JNI_VERSION JNI_VERSION_1_4

void JPRef_failed()
{
	JP_RAISE(PyExc_SystemError, "NULL context in JPRef()");
}

JPContext::JPContext()
{
	m_JavaVM = 0;
	_void = 0;
	_byte = 0;
	_boolean = 0;
	_char = 0;
	_short = 0;
	_int = 0;
	_long = 0;
	_float = 0;
	_double = 0;

	_java_lang_Void = 0;
	_java_lang_Boolean = 0;
	_java_lang_Byte = 0;
	_java_lang_Character = 0;
	_java_lang_Short = 0;
	_java_lang_Integer = 0;
	_java_lang_Long = 0;
	_java_lang_Float = 0;
	_java_lang_Double = 0;

	_java_lang_Object = 0;
	_java_lang_Class = 0;
	_java_lang_String = 0;

	_java_lang_reflect_Method = 0;
	_java_lang_reflect_Field = 0;
	_java_nio_ByteBuffer = 0;

	m_TypeManager = 0;
	m_ClassLoader = 0;

	m_Object_ToStringID = 0;
	m_Object_EqualsID = 0;
	m_Running = false;

	// Java Functions
	m_Object_ToStringID = NULL;
	m_Object_EqualsID = NULL;
	m_Object_HashCodeID = NULL;
	m_CallMethodID = NULL;
	m_Class_GetNameID = NULL;
	m_Context_collectRectangularID = NULL;
	m_Context_assembleID = NULL;
	m_String_ToCharArrayID = NULL;
	m_Context_CreateExceptionID = NULL;
	m_Context_GetExcClassID = NULL;
	m_Context_GetExcValueID = NULL;
	m_CompareToID = NULL;
	m_Buffer_IsReadOnlyID = NULL;
	m_Context_OrderID = NULL;
	m_Object_GetClassID = NULL;
	m_Throwable_GetCauseID = NULL;
	m_Context_GetStackFrameID = NULL;
	m_Embedded = false;
	m_ShutdownMethodID = 0;

	m_GC = new JPGarbageCollection(this);
}

JPContext::~JPContext()
{
	delete m_TypeManager;
	delete m_GC;
}

bool JPContext::isRunning()
{
	if (m_JavaVM == NULL || !m_Running)
	{
		return false;
	}
	return true;
}

/**
	throw a JPypeException if the JVM is not started
 */
void assertJVMRunning(JPContext* context, const JPStackInfo& info)
{
	if (_JVMNotRunning == NULL)
	{
		_JVMNotRunning = PyObject_GetAttrString(PyJPModule, "JVMNotRunning");
		JP_PY_CHECK();
		Py_INCREF(_JVMNotRunning);
	}

	if (context == NULL)
	{
		throw JPypeException(JPError::_python_exc, _JVMNotRunning, "Java Context is null", info);
	}

	if (!context->isRunning())
	{
		throw JPypeException(JPError::_python_exc, _JVMNotRunning, "Java Virtual Machine is not running", info);
	}
}

void JPContext::loadEntryPoints(const string& path)
{
	JP_TRACE_IN("JPContext::loadEntryPoints");
	JPPlatformAdapter *platform = JPPlatformAdapter::getAdapter();
	// Load symbols from the shared library
	platform->loadLibrary((char*) path.c_str());
	CreateJVM_Method = (jint(JNICALL *)(JavaVM **, void **, void *) )platform->getSymbol("JNI_CreateJavaVM");
	GetCreatedJVMs_Method = (jint(JNICALL *)(JavaVM **, jsize, jsize*))platform->getSymbol("JNI_GetCreatedJavaVMs");
	JP_TRACE_OUT;
}

void JPContext::startJVM(const string& vmPath, const StringVector& args,
		bool ignoreUnrecognized, bool convertStrings, bool interrupt)
{
	JP_TRACE_IN("JPContext::startJVM");

	JP_TRACE("Convert strings", convertStrings);
	m_ConvertStrings = convertStrings;

	// Get the entry points in the shared library
	try
	{
		JP_TRACE("Load entry points");
		loadEntryPoints(vmPath);
	} catch (JPypeException& ex)
	{
		ex.getMessage();
		throw;
	}

	// Pack the arguments
	JP_TRACE("Pack arguments");
	JavaVMInitArgs jniArgs;
	jniArgs.options = NULL;

	// prepare this ...
	jniArgs.version = USE_JNI_VERSION;
	jniArgs.ignoreUnrecognized = ignoreUnrecognized;
	JP_TRACE("IgnoreUnrecognized", ignoreUnrecognized);

	jniArgs.nOptions = (jint) args.size();
	JP_TRACE("NumOptions", jniArgs.nOptions);
	jniArgs.options = new JavaVMOption[jniArgs.nOptions];
	memset(jniArgs.options, 0, sizeof (JavaVMOption) * jniArgs.nOptions);
	for (int i = 0; i < jniArgs.nOptions; i++)
	{
		JP_TRACE("Option", args[i]);
		jniArgs.options[i].optionString = (char*) args[i].c_str();
	}

	// Launch the JVM
	JNIEnv* env = NULL;
	JP_TRACE("Create JVM");
	try
	{
		CreateJVM_Method(&m_JavaVM, (void**) &env, (void*) &jniArgs);
	} catch (...)
	{
		JP_TRACE("Exception in CreateJVM?");
	}
	JP_TRACE("JVM created");
	delete [] jniArgs.options;

	if (m_JavaVM == NULL)
	{
		JP_TRACE("Unable to start");
		JP_RAISE(PyExc_RuntimeError, "Unable to start JVM");
	}

	initializeResources(env, interrupt);
	JP_TRACE_OUT;
}

void JPContext::attachJVM(JNIEnv* env)
{
	env->GetJavaVM(&m_JavaVM);
#ifndef ANDROID
	m_Embedded = true;
#endif
	initializeResources(env, false);
}

void JPContext::initializeResources(JNIEnv* env, bool interrupt)
{
	JPJavaFrame frame = JPJavaFrame::external(this, env);
	// This is the only frame that we can use until the system
	// is initialized.  Any other frame creation will result in an error.

	jclass throwableClass = (jclass) frame.FindClass("java/lang/Throwable");
	m_Throwable_GetCauseID = frame.GetMethodID(throwableClass, "getCause", "()Ljava/lang/Throwable;");
	m_Throwable_GetMessageID = frame.GetMethodID(throwableClass, "getMessage", "()Ljava/lang/String;");

	// After the JVM is created but before the context is started, we need
	// to set up all the services that the context will need.
	JP_TRACE("Initialize");

	// We need these first because if anything goes south this is the first
	// thing that will get hit.
	jclass objectClass = frame.FindClass("java/lang/Object");
	m_Object_ToStringID = frame.GetMethodID(objectClass, "toString", "()Ljava/lang/String;");
	m_Object_EqualsID = frame.GetMethodID(objectClass, "equals", "(Ljava/lang/Object;)Z");
	m_Object_HashCodeID = frame.GetMethodID(objectClass, "hashCode", "()I");
	m_Object_GetClassID = frame.GetMethodID(objectClass, "getClass", "()Ljava/lang/Class;");

	m_NoSuchMethodError = JPClassRef(frame, (jclass) frame.FindClass("java/lang/NoSuchMethodError"));
	m_RuntimeException = JPClassRef(frame, (jclass) frame.FindClass("java/lang/RuntimeException"));

	jclass stringClass = frame.FindClass("java/lang/String");
	m_String_ToCharArrayID = frame.GetMethodID(stringClass, "toCharArray", "()[C");

	jclass classClass = frame.FindClass("java/lang/Class");
	m_Class_GetNameID = frame.GetMethodID(classClass, "getName", "()Ljava/lang/String;");

	// Bootloader needs to go first so we can load classes
	m_ClassLoader = new JPClassLoader(frame);

	JP_TRACE("Install native");
	// Start the rest of the services
	m_TypeManager = new JPTypeManager(frame);

	// Prepare to launch
	JP_TRACE("Start Context");
	m_ContextClass = JPClassRef(frame, (jclass) m_ClassLoader->findClass(frame, "org.jpype.JPypeContext"));
	jclass contextClass = m_ContextClass.get();
	m_Context_GetStackFrameID = frame.GetMethodID(contextClass, "getStackTrace",
			"(Ljava/lang/Throwable;Ljava/lang/Throwable;)[Ljava/lang/Object;");

	jmethodID startMethod = frame.GetStaticMethodID(contextClass, "createContext",
			"(JLjava/lang/ClassLoader;Ljava/lang/String;Z)Lorg/jpype/JPypeContext;");
    m_ShutdownMethodID = frame.GetMethodID(cls, "shutdown", "()V");

	// Find the native library
	JPPyObject import = JPPyObject::call(PyImport_AddModule("importlib.util"));
	JPPyObject jpype = JPPyObject::call(PyObject_CallMethod(import.get(), "find_spec", "s", "_jpype"));
	JPPyObject origin = JPPyObject::call(PyObject_GetAttrString(jpype.get(), "origin"));

	// Launch
	jvalue val[4];
	val[0].j = (jlong) this;
	val[1].l = m_ClassLoader->getBootLoader();
	val[2].l = 0;
	val[3].z = interrupt;

	if (!m_Embedded)
	{
		PyObject *import = PyImport_AddModule("importlib.util");
		JPPyObject jpype = JPPyObject::call(PyObject_CallMethod(import, "find_spec", "s", "_jpype"));
		JPPyObject origin = JPPyObject::call(PyObject_GetAttrString(jpype.get(), "origin"));
		val[2].l = frame.fromStringUTF8(JPPyString::asStringUTF8(origin.get()));
	}
	m_JavaContext = JPObjectRef(frame, frame.CallStaticObjectMethodA(contextClass, startMethod, val));

	// Post launch
	JP_TRACE("Connect resources");
	// Hook up the type manager
	jmethodID getTypeManager = frame.GetMethodID(contextClass, "getTypeManager",
			"()Lorg/jpype/manager/TypeManager;");
	m_TypeManager->m_JavaTypeManager = JPObjectRef(frame,
			frame.CallObjectMethodA(m_JavaContext.get(), getTypeManager, 0));

	// Set up methods after everything is start so we get better error
	// messages
	m_CallMethodID = frame.GetMethodID(contextClass, "callMethod",
			"(Ljava/lang/reflect/Method;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
	m_Context_collectRectangularID = frame.GetMethodID(contextClass,
			"collectRectangular",
			"(Ljava/lang/Object;)[Ljava/lang/Object;");

	m_Context_assembleID = frame.GetMethodID(contextClass,
			"assemble",
			"([ILjava/lang/Object;)Ljava/lang/Object;");

	m_Context_GetFunctionalID = frame.GetMethodID(contextClass,
			"getFunctional",
			"(Ljava/lang/Class;)Ljava/lang/String;");

	m_Context_CreateExceptionID = frame.GetMethodID(contextClass, "createException",
			"(JJ)Ljava/lang/Exception;");
	m_Context_GetExcClassID = frame.GetMethodID(contextClass, "getExcClass",
			"(Ljava/lang/Throwable;)J");
	m_Context_GetExcValueID = frame.GetMethodID(contextClass, "getExcValue",
			"(Ljava/lang/Throwable;)J");
	m_Context_OrderID = frame.GetMethodID(contextClass, "order", "(Ljava/nio/Buffer;)Z");
	m_Context_IsPackageID = frame.GetMethodID(contextClass, "isPackage", "(Ljava/lang/String;)Z");
	m_Context_GetPackageID = frame.GetMethodID(contextClass, "getPackage", "(Ljava/lang/String;)Lorg/jpype/pkg/JPypePackage;");
	m_Context_ClearInterruptID = frame.GetStaticMethodID(contextClass, "clearInterrupt", "(Z)V");

	jclass packageClass = m_ClassLoader->findClass(frame, "org.jpype.pkg.JPypePackage");
	m_Package_GetObjectID = frame.GetMethodID(packageClass, "getObject",
			"(Ljava/lang/String;)Ljava/lang/Object;");
	m_Package_GetContentsID = frame.GetMethodID(packageClass, "getContents",
			"()[Ljava/lang/String;");
	m_Context_NewWrapperID = frame.GetMethodID(contextClass, "newWrapper",
			"(J)V");

	m_Array = JPClassRef(frame, frame.FindClass("java/lang/reflect/Array"));
	m_Array_NewInstanceID = frame.GetStaticMethodID(m_Array.get(), "newInstance",
			"(Ljava/lang/Class;[I)Ljava/lang/Object;");

	jclass bufferClass = frame.FindClass("java/nio/Buffer");
	m_Buffer_IsReadOnlyID = frame.GetMethodID(bufferClass, "isReadOnly",
			"()Z");

	jclass comparableClass = frame.FindClass("java/lang/Comparable");
	m_CompareToID = frame.GetMethodID(comparableClass, "compareTo",
			"(Ljava/lang/Object;)I");

	jclass proxyClass = getClassLoader()->findClass(frame, "org.jpype.proxy.JPypeProxy");
	m_ProxyClass = JPClassRef(frame, proxyClass);
	m_Proxy_NewID = frame.GetStaticMethodID(m_ProxyClass.get(),
			"newProxy",
			"(Lorg/jpype/JPypeContext;JJ[Ljava/lang/Class;)Lorg/jpype/proxy/JPypeProxy;");
	m_Proxy_NewInstanceID = frame.GetMethodID(m_ProxyClass.get(),
			"newInstance",
			"()Ljava/lang/Object;");

	m_GC->init(frame);

	_java_nio_ByteBuffer = this->getTypeManager()->findClassByName("java.nio.ByteBuffer");

	// Testing code to make sure C++ exceptions are handled.
	// FIXME find a way to call this from instrumentation.
	// throw std::runtime_error("Failed");
	// Everything is started.
	m_Running = true;
}

void JPContext::onShutdown()
{
	m_Running = false;
}

void JPContext::shutdownJVM(bool disgracefulTermination)
{
	JP_TRACE_IN("JPContext::shutdown");
	if (m_JavaVM == NULL)
		JP_RAISE(PyExc_RuntimeError, "Attempt to shutdown without a live JVM");
	//	if (m_Embedded)
	//		JP_RAISE(PyExc_RuntimeError, "Cannot shutdown from embedded Python");

    // Old style version to force quit with all the risks discussed
    if (disgracefulTermination) {
        {
            JPJavaFrame frame(this);
            JP_TRACE("Shutdown services");
            JP_TRACE(m_JavaContext.get());
            JP_TRACE(m_ShutdownMethodID);

            // Tell Java to shutdown the context
            {
                JPPyCallRelease release;
                if (m_JavaContext.get() != 0)
                    frame.CallVoidMethodA(m_JavaContext.get(), m_ShutdownMethodID, 0);
            }
        }

        // Wait for all non-demon threads to terminate
        // DestroyJVM is rather misnamed.  It is simply a wait call
        // Our reference queue thunk does not appear to have properly set
        // as daemon so we hang here
        JP_TRACE("Destroy JVM");
        //	s_JavaVM->functions->DestroyJavaVM(s_JavaVM);
    }

    // This is the good new version with a save quit. This just has the single
    // culprit that it will hang on deadlocked java threads!
    else {
        // Wait for all non-demon threads to terminate
        JP_TRACE("Destroy JVM");
        {
            JPPyCallRelease call;
            m_JavaVM->DestroyJavaVM();
        }

        JP_TRACE("Delete resources");
        for (std::list<JPResource*>::iterator iter = m_Resources.begin();
                iter != m_Resources.end(); ++iter)
        {
            delete *iter;
        }
        m_Resources.clear();
    }

    // From here on its the same in each version again...
	// unload the jvm library
	JP_TRACE("Unload JVM");
	m_JavaVM = NULL;
	JPPlatformAdapter::getAdapter()->unloadLibrary();
	JP_TRACE_OUT;
}

void JPContext::ReleaseGlobalRef(jobject obj)
{
	JP_TRACE_IN("JPContext::ReleaseGlobalRef", obj);
	// Check if the JVM is already shutdown
	if (m_JavaVM == NULL)
		return;

	// Get the environment and release the resource if we can.
	// Do not attach the thread if called from an unattached thread it is
	// likely a shutdown anyway.
	JNIEnv* env;
	jint res = m_JavaVM->functions->GetEnv(m_JavaVM, (void**) &env, USE_JNI_VERSION);
	if (res != JNI_EDETACHED)
		env->functions->DeleteGlobalRef(env, obj);
	JP_TRACE_OUT;
}

/*****************************************************************************/
// Thread code

void JPContext::attachCurrentThread()
{
	JNIEnv* env;
	jint res = m_JavaVM->functions->AttachCurrentThread(m_JavaVM, (void**) &env, NULL);
	if (res != JNI_OK)
		JP_RAISE(PyExc_RuntimeError, "Unable to attach to thread");
}

void JPContext::attachCurrentThreadAsDaemon()
{
	JNIEnv* env;
	jint res = m_JavaVM->functions->AttachCurrentThreadAsDaemon(m_JavaVM, (void**) &env, NULL);
	if (res != JNI_OK)
		JP_RAISE(PyExc_RuntimeError, "Unable to attach to thread as daemon");
}

bool JPContext::isThreadAttached()
{
	JNIEnv* env;
	return JNI_OK == m_JavaVM->functions->GetEnv(m_JavaVM, (void**) &env, USE_JNI_VERSION);
}

void JPContext::detachCurrentThread()
{
	m_JavaVM->functions->DetachCurrentThread(m_JavaVM);
}

JNIEnv* JPContext::getEnv()
{
	JNIEnv* env = NULL;
	if (m_JavaVM == NULL)
	{
		JP_RAISE(PyExc_RuntimeError, "JVM is null");
	}

	// Get the environment
	jint res = m_JavaVM->functions->GetEnv(m_JavaVM, (void**) &env, USE_JNI_VERSION);

	// If we don't have an environment then we are in a thread, so we must attach
	if (res == JNI_EDETACHED)
	{
		// We will attach as daemon so that the newly attached thread does
		// not deadlock the shutdown.  The user can convert later if they want.
		res = m_JavaVM->AttachCurrentThreadAsDaemon((void**) &env, NULL);
		if (res != JNI_OK)
			JP_RAISE(PyExc_RuntimeError, "Unable to attach to local thread");
	}
	return env;
}

extern "C" JNIEXPORT void JNICALL Java_org_jpype_JPypeContext_onShutdown
(JNIEnv *env, jobject obj, jlong contextPtr)
{
	((JPContext*) contextPtr)->onShutdown();
}

/**********************************************************************
 * Interrupts are complex.   Both Java and Python want to handle the
 * interrupt, but only one can be in control.  Java starts later and
 * installs its handler over Python as a chain.  If Java handles it then
 * the JVM will terminate which leaves Python with a bunch of bad
 * references which tends to lead to segfaults.  So we need to disable
 * the Java one by routing it back to Python.  But if we do so then
 * Java wont respect Ctrl+C.  So we need to handle the interrupt, convert
 * it to a wait interrupt so that Java can break at the next I/O and
 * then trip Python signal handler so the Python gets the interrupt.
 *
 * But this leads to a few race conditions.
 *
 * If the control is in Java then it will get the interrupt next time
 * it hits Python code when the returned object is checked resulting
 * InterruptedException.  Now we have two exceptions on the stack,
 * the one from Java and the one from Python.  We check to see if
 * Python has a pending interrupt and eat the Java one.
 *
 * If the control is in Java and it hits an I/O call.  This generates
 * InterruptedException which again transfers control to Python where
 * the Exception is resolved.
 *
 * If the control is in Python when the interrupt occurs, then
 * we have a bogus Java interrupt sitting on the main thread that the next
 * Java call will trip over.  So we need to call clearInterrupt(false).
 * This checks clears the interrupt in C++ and in Java.
 *
 */

static int interruptState = 0;
extern "C" JNIEXPORT void JNICALL Java_org_jpype_JPypeSignal_interruptPy
(JNIEnv *env, jclass cls)
{
	interruptState = 1;
	PyErr_SetInterrupt();
}

extern "C" JNIEXPORT void JNICALL Java_org_jpype_JPypeSignal_acknowledgePy
(JNIEnv *env, jclass cls)
{
	interruptState = 0;
}

int hasInterrupt()
{
	return interruptState != 0;
}
