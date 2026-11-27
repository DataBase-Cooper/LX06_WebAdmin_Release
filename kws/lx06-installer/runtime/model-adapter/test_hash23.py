"""Frozen old C, independent Python Keccak and optimized C comparison.

No real challenge, credentials or network. Binary messages exercise rate and
padding boundaries; synthetic solver targets verify decimal carries and limits.
"""
import ctypes
import random
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent
MASK=(1<<64)-1
ROT=[0,1,62,28,27,36,44,6,55,20,3,10,43,25,39,41,45,15,21,8,18,2,61,56,14]
RC=[1,0x8082,0x800000000000808a,0x8000000080008000,0x808b,0x80000001,0x8000000080008081,0x8000000000008009,0x8a,0x88,0x80008009,0x8000000a,0x8000808b,0x800000000000008b,0x8000000000008089,0x8000000000008003,0x8000000000008002,0x8000000000000080,0x800a,0x800000008000000a,0x8000000080008081,0x8000000000008080,0x80000001,0x8000000080008008]

def python_digest(message):
    def rol(v,n):return ((v<<n)|(v>>(64-n)))&MASK if n else v
    data=bytearray(message);data.append(6)
    data.extend(bytes((-len(data))%136));data[-1]|=128
    state=[0]*25
    for offset in range(0,len(data),136):
        for lane in range(17):state[lane]^=int.from_bytes(data[offset+lane*8:offset+lane*8+8],'little')
        for constant in RC[1:]:
            columns=[state[x]^state[x+5]^state[x+10]^state[x+15]^state[x+20] for x in range(5)]
            theta=[columns[(x-1)%5]^rol(columns[(x+1)%5],1) for x in range(5)]
            lanes=[0]*25
            for y in range(5):
                for x in range(5):lanes[y+5*((2*x+3*y)%5)]=rol(state[x+5*y]^theta[x],ROT[x+5*y])
            state=[lanes[x+5*y]^((~lanes[(x+1)%5+5*y])&lanes[(x+2)%5+5*y]) for y in range(5) for x in range(5)]
            state[0]^=constant
    return b''.join(lane.to_bytes(8,'little') for lane in state[:4])

class Hash23Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.p=Path(cls.tmp.name)
        wrapper=cls.p/'comparison.c'
        wrapper.write_text('''#include "hash23.h"
#include "tests/hash23-reference.h"
void optimized_digest(const unsigned char *p,size_t n,unsigned char *o){hash23_digest(p,n,o);}
void original_digest(const unsigned char *p,size_t n,unsigned char *o){reference_hash23_digest(p,n,o);}
int optimized_solve(const char*s,const char*e,unsigned n,const unsigned char*t){return hash23_solve(s,e,n,t);}
int original_solve(const char*s,const char*e,unsigned n,const unsigned char*t){return reference_hash23_solve(s,e,n,t);}
''')
        library=cls.p/'comparison.so'
        subprocess.run(['gcc','-O2','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(ROOT),str(wrapper),'-o',str(library)],check=True)
        cls.lib=ctypes.CDLL(str(library))
        for name in ['optimized_digest','original_digest']:
            fn=getattr(cls.lib,name);fn.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p];fn.restype=None
        for name in ['optimized_solve','original_solve']:
            fn=getattr(cls.lib,name);fn.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint,ctypes.c_void_p];fn.restype=ctypes.c_int
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def digest(self,name,data):
        result=ctypes.create_string_buffer(32);getattr(self.lib,name)(data,len(data),result);return result.raw
    def solve(self,name,salt,expiry,limit,target):return getattr(self.lib,name)(salt.encode(),expiry.encode(),limit,target)

    def test_known_vectors_against_both_c_and_independent_python(self):
        vectors={b'':'e594808bc5b7151ac160c6d39a02e0a8e261ed588578403099e3561dc40c26b3',
                 b'testsalt_1700000000_42':'d4a2ea58c89e40887c933484868380c6f803eaa8dc53a3b9df8e431b921a4f09',
                 b'testsalt_1700000000_100000':'abea2f35796b65486e9be1b36f7878c66cab021e96faa473fdf4decd31f9ba30',
                 b'abc123salt_1700000000_12345':'74b3b7452745b70e85eb32ee7f0a9ec0381d42dd5137b695da915e104fc390e1'}
        for data,expected in vectors.items():
            for name in ['optimized_digest','original_digest']:self.assertEqual(self.digest(name,data).hex(),expected)
            self.assertEqual(python_digest(data).hex(),expected)

    def test_random_binary_and_multiblock_padding_boundaries(self):
        rng=random.Random(311023)
        sizes=[0,1,7,8,9,127,128,129,130,134,135,136,137,143,199,200,271,272,273,407,408,409,1024,4096]
        sizes.extend(rng.randrange(0,2050) for _ in range(128))
        for n in sizes:
            data=rng.randbytes(n);new=self.digest('optimized_digest',data)
            self.assertEqual(new,self.digest('original_digest',data),n)
            self.assertEqual(new,python_digest(data),n)

    def test_solver_decimal_carries_rate_boundaries_and_random_targets(self):
        rng=random.Random(23136);expiry='1700000000000'
        cases=[('B'*(prefix-15),answer) for prefix in [15,32,128,129,130,131,132,133,134,135,136,137,199,200] for answer in [0,9,10,99,100,999,1000]]
        cases += [('synthetic-prefix',answer) for answer in [9999,10000,99999,100000,199999]]
        # Six-digit carry crossed with the 136-byte rate boundary: prefix130
        # plus "100000" requires the extra SHA3 padding block. Testing these
        # dimensions separately would miss an off-by-one fast-path condition.
        cases += [('B'*(prefix-15),answer) for prefix in [129,130,131] for answer in [99999,100000]]
        cases += [(''.join(rng.choice('aZ01._-') for _ in range(rng.randrange(0,180))),rng.randrange(0,3000)) for _ in range(32)]
        for salt,answer in cases:
            target=python_digest(f'{salt}_{expiry}_{answer}'.encode())
            for name in ['optimized_solve','original_solve']:
                self.assertEqual(self.solve(name,salt,expiry,answer+1,target),answer,(name,len(salt),answer))

    def test_no_match_and_input_budget_bounds_remain_identical(self):
        expiry='1700000000000';target=python_digest(b'synthetic_1700000000000_17')
        for name in ['optimized_solve','original_solve']:
            self.assertEqual(self.solve(name,'synthetic',expiry,17,target),-1)
            for limit in [0,200001,4294967295]:self.assertEqual(self.solve(name,'synthetic',expiry,limit,target),-1)
            self.assertEqual(self.solve(name,'B'*201,expiry,20,target),-1)

    def test_fifteen_second_clock_and_nice_fifteen_remain_enforced(self):
        source=self.p/'budget.c';binary=self.p/'budget'
        source.write_text('''#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <sys/resource.h>
#include <stdio.h>
static int calls,priority;
static int fake_clock(clockid_t clock,struct timespec *value){(void)clock;value->tv_sec=calls++<2?100:115;value->tv_nsec=0;return 0;}
static int fake_priority(__priority_which_t which,id_t who,int value){(void)which;(void)who;priority=value;return 0;}
#define clock_gettime fake_clock
#define setpriority fake_priority
#include "hash23.h"
int main(void){const char*m="synthetic_1700000000000_300";unsigned char target[32];hash23_digest((const unsigned char*)m,strlen(m),target);int r=hash23_solve("synthetic","1700000000000",301,target);printf("%d %d %d",r,priority,calls);return r==-1&&priority==15&&calls==3?0:1;}
''')
        subprocess.run(['gcc','-O2','-I'+str(ROOT),str(source),'-o',str(binary)],check=True)
        r=subprocess.run([str(binary)],capture_output=True,text=True,timeout=2)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertEqual(r.stdout,'-1 15 3')

if __name__=='__main__':unittest.main()
