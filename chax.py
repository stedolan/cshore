import sys, os, os.path, re, imp, json, ctypes
from collections import defaultdict

LIBRARIES = ['libglut.so', 'libGL.so', 'libGLU.so']

LOADED_LIBRARIES = {}

class EnumerationMixin(object):
    def __init__(self, name, value):
        self.name = name
        super(EnumerationMixin, self).__init__(value)
    def __repr__(self):
        if self.__class__.__name__:
            return "<%s (%d) of %s>" % (self.name, self.value, self.__class__.__name__)
        else:
            return "<%s (%d)>" % (self.name, self.value)

class CLoader:
    def __init__(self, json):
        self.types = {}
        self.typedefs = {}
        self.complex_types = {}
        self.struct = []
        self.union = []
        self.enum = []
        self.macros = []
        self.objects = []
        self.recursive_typedef_check = set()
        if "" in json:
            del json[""]
        self.json = json

        objects_by_kind = defaultdict(lambda:[])
        for id, obj in self.json.items():
            objects_by_kind[obj['kind']].append(id)
        phases = [
            (self.register_macro, ["macro"]),
            (self.register_complex_type, ["struct", "union"]),
            (self.register_enum, ["enum"]),
            (self.register_typedef, ["typedef"]),
            (self.init_complex_type, ["struct", "union"]),
            (self.register_function, ["function"])] # FIXME: global variables?
        for action, kinds in phases:
            #print action
            for id in [x for k in kinds for x in objects_by_kind[k]]:
                #print id
                action(id, self.json[id])

    def module_dict(self):
        d = {}
        d['_macro_'] = self.macros
        d['_struct_'] = self.struct
        d['_union_'] = self.union
        d['_enum_'] = self.enum
        d['_typedef_'] = self.typedefs
        d.update(self.enum)
        for name, enum in self.enum:
            d.update(dict((v.name, v) for v in enum._values_))
        d.update(self.macros)
        d.update(self.typedefs)
        d.update(self.union)
        d.update(self.struct)
        d.update(self.objects)
        return d

    def register_macro(self, id, obj):
        self.macros.append((obj['name'], obj.get('value')))
    
    def register_complex_type(self, id, obj):
        basecls = {"struct": ctypes.Structure,
                   "union": ctypes.Union}[obj['kind']]
        name = obj['name'] or "<unnamed %s>" % obj['kind']
        cls = type(str(name), (basecls, ), {})
        dict = getattr(self, obj['kind'])
        dict.append((name, cls))
        self.types[id] = cls
        self.complex_types[cls] = id

    def register_enum(self, id, obj):
        if obj['values']:
            ctypes = set(self.get_ctype(v['type']) for v in obj['values'])
            if len(ctypes) != 1:
                raise ImportError("Strange enum types for %s" % obj['name'])
            ctype = list(ctypes)[0]
        else:
            # for empty enums
            ctype = ctypes.c_int
            
        cls = type(str(obj['name']), (EnumerationMixin, ctype), {})
        values = [cls(v['name'], v['value']) for v in obj['values']]
        cls._values_ = values
        for val in values:
            setattr(cls, val.name, val)
        self.enum.append((obj['name'],cls))
        self.types[id] = cls

    def register_typedef(self, id, obj):
        if id in self.types:
            # already registered via typedef crossreferences
            return
        ctype = self.get_ctype(obj['type'])
        self.typedefs[obj['name']] = ctype
        self.types[id] = ctype

    def init_complex_type(self, id, obj):
        if hasattr(self.types[id], '_fields_'):
            return # already initialised
        fields = []
        for f in obj['fields']:
            try:
                ctype = self.get_ctype(f['type'])
            except ImportError, ex:
                print ex
                break
            if ctype in self.complex_types:
                ctype_id = self.complex_types[ctype]
                self.init_complex_type(ctype_id, self.json[ctype_id])
            fields.append((f['name'], ctype))
        self.types[id]._fields_ = fields

    def register_function(self, id, obj):
        name = obj['name']
        ctype = self.get_ctype(obj['type'])
        ptr = None
        for libname in LIBRARIES:
            if libname not in LOADED_LIBRARIES:
                LOADED_LIBRARIES[libname] = ctypes.cdll.LoadLibrary(libname)
            lib = LOADED_LIBRARIES[libname]
            if hasattr(lib, name):
                ptr = getattr(lib, name)
                break
        #print name, ptr, json.dumps(obj, indent=2)
        if ptr:
            ptr.restype = ctype(0).restype
            ptr.argtypes = ctype(0).argtypes
            self.objects.append((name, ptr))
        
    PRIMITIVE_CTYPES = {
        'void': None,
        'bool': ctypes.c_bool,
        'char': ctypes.c_char,
        'wchar_t': ctypes.c_wchar,
        'signed char': ctypes.c_byte,
        'unsigned char': ctypes.c_ubyte,
        'short': ctypes.c_short,
        'unsigned short': ctypes.c_ushort,
        'int': ctypes.c_int,
        'unsigned int': ctypes.c_uint,
        'long': ctypes.c_long,
        'unsigned long': ctypes.c_ulong,
        'long long': ctypes.c_longlong,
        'unsigned long long': ctypes.c_ulonglong,
        'float': ctypes.c_float,
        'double': ctypes.c_double,
        'long double': ctypes.c_longdouble
        }
    def get_ctype(self, type):
        if type['kind'] == 'primitive':
            if type['primitive'] in CLoader.PRIMITIVE_CTYPES:
                return CLoader.PRIMITIVE_CTYPES[type['primitive']]
            else:
                raise ImportError("Unknown primitive type %s" % type['primitive'])
        elif type['kind'] == 'function':
            ret = self.get_ctype(type['return'])
            args = [self.get_ctype(t) for t in type['arguments']]
            return ctypes.CFUNCTYPE(ret, *args)
        elif type['kind'] == 'pointer':
            pointee = self.get_ctype(type['pointee'])
            if pointee == ctypes.c_char:
                return ctypes.c_char_p
            elif pointee == ctypes.c_wchar:
                return ctypes.c_wchar_p
            elif pointee == None:
                return ctypes.c_void_p
            elif hasattr(pointee, 'restype'):
                return pointee
            else:
                return ctypes.POINTER(pointee)
        elif type['kind'] == 'array':
            if 'length' in type:
                return self.get_ctype(type['element']) * type['length']
            else:
                raise ImportError("Array of unknown length")
        elif type['kind'] == 'ref':
            if type['id'] not in self.json:
                raise ImportError("Type %s doesn't exist" % type['id'])
            if type['id'] not in self.types:
                if type['id'] in self.recursive_typedef_check:
                    raise ImportError("Recursive typedefs")
                self.recursive_typedef_check.add(type['id'])
                self.register_typedef(type['id'], self.json[type['id']])
                self.recursive_typedef_check.remove(type['id'])
            return self.types[type['id']]
        else:
            raise ImportError("Can't parse type %r" % type)
    
    

def list_add(l, *xs):
    for x in xs:
        if x not in l:
            l.append(x)

list_add(sys.path, "$C$/usr/include", "$C$/usr/include/x86_64-linux-gnu/")


        
class CFinder:
    def __init__(self, syspath):
        if syspath.startswith("$C$"):
            self.pythonpath = syspath
            self.path = syspath[3:]
        else:
            raise ImportError
    def find_module(self, fullname, mpath = None):
        parts = fullname.split(".")

        if parts[0] != "c":
            return None

        if len(parts) == 1:
            return self
        path = os.path.join(self.path, "/".join(parts[1:]))
        if os.path.isdir(path) or os.path.isfile(path + ".h"):
            return self

    def load_module(self, fullname):
        path = os.path.join(self.path, "/".join(fullname.split(".")[1:]))
        print "Loading " + fullname + " from " + path
        mod = sys.modules.setdefault(fullname, imp.new_module(fullname))
        mod.__loader__ = self
        if fullname == "c":
            mod.__file__ = None
            mod.__path__ = sys.path
            mod.__package__ = fullname
        elif os.path.isdir(path):
            mod.__file__ = path
            mod.__path__ = [self.pythonpath]
            mod.__package__ = fullname
        elif os.path.isfile(path + ".h"):
            mod.__file__ = path + ".h"
            mod.__path__ = []
            mod.__package__ = fullname.rpartition(".")[0]
            mod.__dict__.update(CLoader(json.load(open("test.json"))).module_dict())
            #mod.__dict__.update(gen_c_module(json.load(open("test.json"))))
        else:
            raise ImportError
        return mod

        

list_add(sys.path_hooks, CFinder)

import c
import c.sys.types

from c.sys.types import *

#print c
#print c.sys, c.sys.__package__
#print c.X11.Xauth, c.X11.Xauth.__package__

#print c.sys.types.__dict__.keys()
#print c.sys.types.int16_t

#print c.sys.types.__fsid_t
#print c.sys.types.__fsid_t._fields_
#print c.sys.types.SCM_RIGHTS
#print c.sys.types.__pthread_mutex_s._fields_

#print c.sys.types.stat
#print c.sys.types.fstat
#statbuf = c.sys.types.stat()
#print c.sys.types.stat
#print c.sys.types.fstat(42, ctypes.byref(statbuf))
#print statbuf

from ctypes import *


glutInit(byref(c_int(0)), None)
glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH)
glutCreateWindow("red 3D lighted cube")


Vector4 = GLfloat * 4
Vector3 = GLfloat * 3

light_diffuse = Vector4(1.0, 0.0, 0.0, 1.0)
light_position = Vector4(1.0, 1.0, 1.0, 0.0)

v = (Vector3 * 8)()
v[0][0], v[1][0], v[2][0], v[3][0] = -1, -1, -1, -1
v[4][0], v[5][0], v[6][0], v[7][0] = 1, 1, 1, 1

v[0][1], v[1][1], v[4][1], v[5][1] = -1, -1, -1, -1
v[2][1], v[3][1], v[6][1], v[7][1] = 1, 1, 1, 1

v[0][2], v[3][2], v[4][2], v[7][2] = 1, 1, 1, 1
v[1][2], v[2][2], v[5][2], v[6][2] = -1, -1, -1, -1

faces = [(0, 1, 2, 3),
         (3, 2, 6, 7),
         (7, 6, 5, 4),
         (4, 5, 1, 0),
         (5, 6, 2, 1),
         (7, 4, 0, 3)]

normals = [
    Vector3(-1.0, 0.0, 0.0),
    Vector3(0.0, 1.0, 0.0),
    Vector3(1.0, 0.0, 0.0),
    Vector3(0.0, -1.0, 0.0), 
    Vector3(0.0, 0.0, 1.0), 
    Vector3(0.0, 0.0, -1.0)]



glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diffuse)
glLightfv(GL_LIGHT0, GL_POSITION, light_position)
glEnable(GL_LIGHT0)
glEnable(GL_LIGHTING)

glEnable(GL_DEPTH_TEST)

glMatrixMode(GL_PROJECTION)
gluPerspective( 40.0, 1.0,1.0, 10.0)
glMatrixMode(GL_MODELVIEW)
gluLookAt(0.0, 0.0, 5.0, 
          0.0, 0.0, 0.0,      
          0.0, 1.0, 0.)      

glTranslatef(0.0, 0.0, -1.0)
glRotatef(60, 1.0, 0.0, 0.0)
glRotatef(-20, 0.0, 0.0, 1.0)

def display():
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
    for i in range(6):
        glBegin(GL_QUADS)
        glNormal3fv(normals[i])
        glVertex3fv(v[faces[i][0]])
        glVertex3fv(v[faces[i][1]])
        glVertex3fv(v[faces[i][2]])
        glVertex3fv(v[faces[i][3]])
        glEnd()
    glutSwapBuffers()

glutDisplayFunc(glutDisplayFunc.argtypes[0](display))

glutMainLoop()
