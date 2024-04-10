/* *INDENT-OFF* */
#ifndef Py_AWAITABLE_H
#define Py_AWAITABLE_H

#ifdef __cplusplus
extern "C" {
#endif


typedef int (*awaitcallback)(PyObject *, PyObject *);
typedef int (*awaitcallback_err)(PyObject *, PyObject *);

typedef struct _PyAwaitableObject PyAwaitableObject;

extern PyTypeObject PyAwaitable_Type;
extern PyTypeObject _PyAwaitable_GenWrapper_Type;

PyAPI_FUNC(PyObject *) PyAwaitable_New(void);

PyAPI_FUNC(int) PyAwaitable_AddAwait(PyObject *aw, PyObject *coro, awaitcallback cb, awaitcallback_err err);
PyAPI_FUNC(void) PyAwaitable_Cancel(PyObject *aw);

PyAPI_FUNC(int) PyAwaitable_SetResult(PyObject *awaitable, PyObject *result);

PyAPI_FUNC(int) PyAwaitable_SaveValuesVa(PyObject *awaitable, Py_ssize_t nargs, ...);
PyAPI_FUNC(int) PyAwaitable_UnpackValuesVa(PyObject *awaitable, ...);

PyAPI_FUNC(int) PyAwaitable_SaveValues(PyObject *awaitable, Py_ssize_t nargs, PyObject **values);
PyAPI_FUNC(int) PyAwaitable_UnpackValues(PyObject *awaitable, PyObject **values);

PyAPI_FUNC(int) PyAwaitable_SaveArbValuesVa(PyObject *awaitable, Py_ssize_t nargs, ...);
PyAPI_FUNC(int) PyAwaitable_UnpackArbValuesVa(PyObject *awaitable, ...);

PyAPI_FUNC(int) PyAwaitable_SaveArbValues(PyObject *awaitable, Py_ssize_t nargs, void **args);
PyAPI_FUNC(int) PyAwaitable_UnpackArbValues(PyObject *awaitable, void **args);

#ifdef __cplusplus
}
#endif

#define PyAwaitable_AWAIT(aw, coro) PyAwaitable_AddAwait(aw, coro, NULL, NULL)
#endif