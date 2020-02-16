/*****************************************************************************
   Copyright 2004-2008 Steve Ménard

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

 *****************************************************************************/
#include "jpype.h"
#include "jp_arrayclass.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * Create a new object.
 *
 * This is only called from the Python side.
 *
 * @param type
 * @param args
 * @param kwargs
 * @return
 */
static PyObject *PyJPArray_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
	JP_PY_TRY("PyJPArray_new");
	PyJPArray* self = (PyJPArray*) type->tp_alloc(type, 0);
	JP_PY_CHECK();
	self->m_Array = NULL;
	self->m_View = NULL;
	return (PyObject*) self;
	JP_PY_CATCH(NULL);
}

static int PyJPArray_init(PyObject *self, PyObject *args, PyObject *kwargs)
{
	JP_PY_TRY("PyJPArray_init");
	ASSERT_JVM_RUNNING();
	JP_TRACE("before");
	JPJavaFrame frame;
	JP_TRACE("after");

	// Cases here.
	//  -  We got here with a JPValue
	//  -  We get an integer. Just create a new array with desired size.
	//  -  We get a sequence. Allocate with desired size and call setItems.
	//  -  We get something else.... ???

	PyObject* v;
	if (!PyArg_ParseTuple(args, "O", &v))
		return -1;

	JPClass *cls = PyJPClass_getJPClass((PyObject*) Py_TYPE(self));
	JPArrayClass* arrayClass = dynamic_cast<JPArrayClass*> (cls);
	if (arrayClass == NULL)
		JP_RAISE(PyExc_TypeError, "Class must be array type");

	JPValue *value = PyJPValue_getJavaSlot(v);
	if (value != NULL)
	{
		JPJavaFrame frame;
		JPArrayClass* arrayClass2 = dynamic_cast<JPArrayClass*> (value->getClass());
		if (arrayClass2 == NULL)
			JP_RAISE(PyExc_TypeError, "Class must be array type");
		if (arrayClass2 != arrayClass)
			JP_RAISE(PyExc_TypeError, "Array class mismatch");
		((PyJPArray*) self)->m_Array = new JPArray(*value);
		PyJPValue_assignJavaSlot(self, *value);
		return 0;
	}

	if (PySequence_Check(v))
	{
		JPJavaFrame frame;
		jlong length =  PySequence_Size(v);
		if (length < 0 || length > 2147483647)
			JP_RAISE(PyExc_ValueError, "Array size invalid");
		JPValue newArray = arrayClass->newInstance(frame, (int) length);
		((PyJPArray*) self)->m_Array = new JPArray(newArray);
		((PyJPArray*) self)->m_Array->setRange(0, (jsize) length, 1, v);
		PyJPValue_assignJavaSlot(self, newArray);
		return 0;
	}

	if (PyIndex_Check(v))
	{
		long long length = PyLong_AsLongLong(v);
		if (length < 0 || length > 2147483647)
			JP_RAISE(PyExc_ValueError, "Array size invalid");
		JPValue newArray = arrayClass->newInstance(frame, (int) length);
		((PyJPArray*) self)->m_Array = new JPArray(newArray);
		PyJPValue_assignJavaSlot(self, newArray);
		return 0;
	}

	JP_RAISE(PyExc_TypeError, "Invalid type");
	JP_PY_CATCH(-1);
}

static void PyJPArray_dealloc(PyJPArray *self)
{
	JP_PY_TRY("PyJPArray_dealloc");
	delete self->m_Array;
	Py_TYPE(self)->tp_free(self);
	JP_PY_CATCH();
}

static PyObject *PyJPArray_repr(PyJPArray *self)
{
	JP_PY_TRY("PyJPArray_repr");
	ASSERT_JVM_RUNNING();
	JPJavaFrame frame;
	if (self->m_Array == NULL)
		JP_RAISE(PyExc_TypeError, "Null array");
	stringstream sout;

	// FIXME way too hard to get this type name.
	sout << "<java array " << self->m_Array->getClass()->toString() << ">";
	return JPPyString::fromStringUTF8(sout.str()).keep();
	JP_PY_CATCH(0);
}

static Py_ssize_t PyJPArray_len(PyJPArray *self)
{
	JP_PY_TRY("PyJPArray_len");
	ASSERT_JVM_RUNNING();
	if (self->m_Array == NULL)
		JP_RAISE(PyExc_RuntimeError, "Null array");
	return self->m_Array->getLength();
	JP_PY_CATCH(-1);
}

static PyObject* PyJPArray_length(PyJPArray *self, PyObject *closure)
{
	return PyLong_FromLong(PyJPArray_len(self));
}

static PyObject *PyJPArray_getItem(PyJPArray *self, PyObject *item)
{
	JP_PY_TRY("PyJPArray_getArrayItem");
	ASSERT_JVM_RUNNING();
	JPJavaFrame frame;
	if (self->m_Array == NULL)
		JP_RAISE(PyExc_TypeError, "Null array");

	if (PyIndex_Check(item))
	{
		Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
		if (i == -1 && PyErr_Occurred())
			return NULL;
		return self->m_Array->getItem((jsize) i).keep();
	}

	if (PySlice_Check(item))
	{
		Py_ssize_t start, stop, step, slicelength;
		Py_ssize_t length = (Py_ssize_t) self->m_Array->getLength();

#if PY_VERSION_HEX<0x03060100
		if (PySlice_GetIndicesEx(item, length, &start, &stop, &step, &slicelength) < 0)
			return NULL;
#else
		if (PySlice_Unpack(item, &start, &stop, &step) < 0)
			return NULL;

		slicelength = PySlice_AdjustIndices(length, &start, &stop, step);
#endif
		if (slicelength <= 0)
		{
			// FIXME this should point to a null array so we don't hold worthless
			// memory, but this is a low priority
			start = stop = 0;
			step = 1;
		}

		JPPyTuple tuple(JPPyTuple::newTuple(0));
		JPPyObject newArray = JPPyObject(JPPyRef::_claim,
				Py_TYPE(self)->tp_new(Py_TYPE(self), tuple.get(), NULL));

		// Copy over the JPValue
		PyJPValue_assignJavaSlot(newArray.get(),
				*PyJPValue_getJavaSlot((PyObject*) self));

		// Set up JPArray as slice
		JPArray *array = ((PyJPArray*) self)->m_Array;
		((PyJPArray*) newArray.get())->m_Array = new JPArray(array,
				(jsize) start, (jsize) stop, (jsize) step);
		return newArray.keep();
	}

	JP_RAISE(PyExc_TypeError, "Unsupported getItem type");
	JP_PY_CATCH(NULL);
}

static int PyJPArray_assignItem(PyJPArray *self, Py_ssize_t item, PyObject *value)
{
	JP_PY_TRY("PyJPArray_assignItem");
	ASSERT_JVM_RUNNING();
	JPJavaFrame frame;
	self->m_Array->setItem((jsize) item, value);
	return 0;
	JP_PY_CATCH(-1);
}

static int PyJPArray_assignSubscript(PyJPArray *self, PyObject *item, PyObject *value)
{
	JP_PY_TRY("PyJPArray_setArrayItem");
	ASSERT_JVM_RUNNING();
	JPJavaFrame frame;
	if ( value == NULL)
		JP_RAISE(PyExc_ValueError, "item deletion not supported");

	// Watch out for self assignment
	if (PyObject_IsInstance(value, (PyObject*) PyJPArray_Type))
	{
		JPValue *v1 = PyJPValue_getJavaSlot((PyObject*) self);
		JPValue *v2 = PyJPValue_getJavaSlot((PyObject*) value);
		if (JPJni::equalsObject(v1->getJavaObject(), v2->getJavaObject()))
			JP_RAISE(PyExc_ValueError, "self assignment not support currently");
	}

	if (PyIndex_Check(item))
	{
		Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
		if (i == -1 && PyErr_Occurred())
			return -1;
		self->m_Array->setItem((jsize) i, value);
		return 0;
	}

	if (PySlice_Check(item))
	{
		Py_ssize_t start, stop, step, slicelength;
		Py_ssize_t length = (Py_ssize_t) self->m_Array->getLength();

#if PY_VERSION_HEX<0x03060100
		if (PySlice_GetIndicesEx(item, length, &start, &stop, &step, &slicelength) < 0)
			return -1;
#else
		if (PySlice_Unpack(item, &start, &stop, &step) < 0)
			return -1;

		slicelength = PySlice_AdjustIndices(length, &start, &stop, step);
#endif
		if (slicelength <= 0)
			return 0;

		self->m_Array->setRange((jsize) start, (jsize) slicelength, (jsize) step,  value);
		return 0;
	}
	PyErr_Format(PyExc_TypeError,
			"Java array indices must be integers or slices, not '%s'",
			Py_TYPE(item)->tp_name);
	JP_PY_CATCH(-1);
}

void PyJPArray_releaseBuffer(PyJPArray *self, Py_buffer *view)
{
	JP_PY_TRY("PyJPArray_releaseBuffer");
	ASSERT_JVM_RUNNING();
	JPJavaFrame frame;
	if (self->m_View == NULL || !self->m_View->unreference())
		return;
	delete self->m_View;
	self->m_View = NULL;
	JP_PY_CATCH();
}

int PyJPArray_getBuffer(PyJPArray *self, Py_buffer *view, int flags)
{
	JP_PY_TRY("PyJPArray_getBuffer");
	ASSERT_JVM_RUNNING();
	JPJavaFrame frame;
	try
	{
		if (self->m_View == NULL)
		{
			self->m_View = new JPArrayView(self->m_Array);
		}
		self->m_View->reference();
		*view = self->m_View->buffer;

		// We are always contiguous so no need to check that here.
		if ((flags & PyBUF_WRITEABLE) != PyBUF_WRITEABLE)
		{
			self->m_View->buffer.readonly = 0;
			view->readonly = 0;
		}

		// If strides are not requested and this is a slice then fail
		if ((flags & PyBUF_STRIDES) != PyBUF_STRIDES)
		{
			if (view->strides[0] != view->itemsize)
				JP_RAISE(PyExc_BufferError, "slices required strides");
			view->strides = NULL;
		}

		// If shape is not requested
		if ((flags & PyBUF_ND) != PyBUF_ND)
		{
			view->shape = NULL;
		}

		// If format is not requested
		if ((flags & PyBUF_FORMAT) != PyBUF_FORMAT)
			view->format = NULL;

		// Okay all successful so reference the parent object
		view->obj = (PyObject*) self;
		Py_INCREF(view->obj);
		return 0;
	}	catch (JPypeException &ex)
	{
		PyJPArray_releaseBuffer(self, view);
		throw ex;
	}
	JP_PY_CATCH(-1);
}

static const char *length_doc =
		"Get the length of a Java array\n"
		"\n"
		"This method is provided for compatibility with Java syntax.\n"
		"Generally, the Python style ``len(array)`` should be preferred.\n";

static PyMethodDef arrayMethods[] = {
	{"__getitem__", (PyCFunction) (&PyJPArray_getItem), METH_O | METH_COEXIST, ""},
	{NULL},
};

static PyGetSetDef arrayGetSets[] = {
	{"length", (getter) (&PyJPArray_length), NULL, const_cast<char*> (length_doc)},
	{0}
};

static PyType_Slot arraySlots[] = {
	{ Py_tp_new,      (void*) PyJPArray_new},
	{ Py_tp_init,     (void*) PyJPArray_init},
	{ Py_tp_dealloc,  (void*) PyJPArray_dealloc},
	{ Py_tp_repr,     (void*) PyJPArray_repr},
	{ Py_tp_methods,  (void*) &arrayMethods},
	{ Py_sq_ass_item, (void*) &PyJPArray_assignItem},
	{ Py_sq_item,     (void*) &PyJPArray_getItem},
	{ Py_sq_length,   (void*) &PyJPArray_len},
	{ Py_mp_ass_subscript, (void*) &PyJPArray_assignSubscript},
	{0}
};

PyTypeObject *PyJPArray_Type = NULL;
static PyType_Spec arraySpec = {
	"_jpype._JArray",
	sizeof (PyJPArray),
	0,
	Py_TPFLAGS_DEFAULT  | Py_TPFLAGS_BASETYPE,
	arraySlots
};

static PyBufferProcs arrayPrimBuffer = {
	(getbufferproc) & PyJPArray_getBuffer,
	(releasebufferproc) & PyJPArray_releaseBuffer
};

static PyType_Slot arrayPrimSlots[] = {
	{0}
};

PyTypeObject *PyJPArrayPrimitive_Type = NULL;
static PyType_Spec arrayPrimSpec = {
	"_jpype._JArrayPrimitive",
	0,
	0,
	Py_TPFLAGS_DEFAULT  | Py_TPFLAGS_BASETYPE,
	arrayPrimSlots
};

#ifdef __cplusplus
}
#endif

void PyJPArray_initType(PyObject *module)
{
	JPPyTuple tuple = JPPyTuple::newTuple(1);
	tuple.setItem(0, (PyObject*) PyJPObject_Type);
	PyJPArray_Type = (PyTypeObject*) PyJPClass_FromSpecWithBases(&arraySpec, tuple.get());
	JP_PY_CHECK();
	PyModule_AddObject(module, "_JArray", (PyObject*) PyJPArray_Type);
	JP_PY_CHECK();

	tuple = JPPyTuple::newTuple(1);
	tuple.setItem(0, (PyObject*) PyJPArray_Type);
	PyJPArrayPrimitive_Type = (PyTypeObject*)
			PyJPClass_FromSpecWithBases(&arrayPrimSpec, tuple.get());
	PyJPArrayPrimitive_Type->tp_as_buffer = &arrayPrimBuffer;
	JP_PY_CHECK();
	PyModule_AddObject(module, "_JArrayPrimitive",
			(PyObject*) PyJPArrayPrimitive_Type);
	JP_PY_CHECK();
}

JPPyObject PyJPArray_create(PyTypeObject *type, JPValue &value)
{
	PyObject *obj = type->tp_alloc(type, 0);
	JP_PY_CHECK();
	((PyJPArray*) obj)->m_Array = new JPArray(value);
	PyJPValue_assignJavaSlot(obj, value);
	return JPPyObject(JPPyRef::_claim, obj);
}
