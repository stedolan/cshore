import ctypes
class A(object):
    def __init__(self, x):
        print "A:", x

class B(object):
    def __init__(self, a, b):
        print "B:", a, b
        super(B, self).__init__(b)

class C(B,A):
    pass

C(42, 45)


class A(ctypes.Structure):
    pass

class B(ctypes.Structure):
    pass

f = []
f.append(('foo', ctypes.POINTER(B)))
A._fields_ = f
B._fields_ = [('bar', ctypes.POINTER(A))]
