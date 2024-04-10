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

PyObject *PyAwaitable_New(void);

int PyAwaitable_AddAwait(PyObject *aw, PyObject *coro, awaitcallback cb, awaitcallback_err err);
void PyAwaitable_Cancel(PyObject *aw);

int PyAwaitable_SetResult(PyObject *awaitable, PyObject *result);

int PyAwaitable_SaveValuesVa(PyObject *awaitable, Py_ssize_t nargs, ...);
int PyAwaitable_UnpackValuesVa(PyObject *awaitable, ...);

int PyAwaitable_SaveValues(PyObject *awaitable, Py_ssize_t nargs, PyObject **values);
int PyAwaitable_UnpackValues(PyObject *awaitable, PyObject **values);

int PyAwaitable_SaveArbValuesVa(PyObject *awaitable, Py_ssize_t nargs, ...);
int PyAwaitable_UnpackArbValuesVa(PyObject *awaitable, ...);

int PyAwaitable_SaveArbValues(PyObject *awaitable, Py_ssize_t nargs, void **args);
int PyAwaitable_UnpackArbValues(PyObject *awaitable, void **args);

#ifdef __cplusplus
}
#endif

#define PyAwaitable_AWAIT(aw, coro) PyAwaitable_AddAwait(aw, coro, NULL, NULL)
#endif