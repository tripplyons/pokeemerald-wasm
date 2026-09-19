#!/usr/bin/env python3
"""Exercise the native C boundary without compiling the game."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PREPARE = Path(__file__).with_name('prepare_source.py').resolve()


class NativeSourceTest(unittest.TestCase):
    def test_pointers_packing_and_zero_memory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'input.i'
            source.write_text(r'''
typedef unsigned int u32;
struct Packed { char tag; void *pointer; } __attribute__((packed));
struct Node { int value; struct Node *next; };
_Static_assert(sizeof(struct Packed) == 1 + sizeof(void *), "packed layout");
_Static_assert(_Alignof(struct Packed) == 1, "packed alignment");
char *zero = (char *)0;
char *address = (char *)4;
int main(void)
{
    struct Node node = {17, &node};
    struct Node *pointer = &node;
    struct Node *nullNode = (struct Node *)0;
    int *nullArray = (int *)0;
    int array[2] = {2, 3};
    u32 word = (u32)pointer->next;
    if ((struct Node *)word != pointer) return 1;
    if (zero != (char *)0 || *address != 0) return 2;
    if (nullNode->value != 0 || nullArray[1] != 0) return 3;
    if (*array != 2 || array[1] != 3) return 4;
    return 0;
}
''')
            output = root / 'prepared.c'
            target_format = 'macho' if sys.platform == 'darwin' else 'elf'
            subprocess.run([sys.executable, str(PREPARE), str(source), str(output),
                            target_format, 'clang'], check=True)
            runtime = root / 'runtime.c'
            runtime.write_text('''
unsigned char gNativeMemory[64];
static const void *saved;
unsigned int NativePointerToWord(const void *pointer) { saved = pointer; return 0x10000001; }
void *NativeDecodePointer(unsigned long word) { return (void *)saved; }
''')
            executable = root / 'test'
            subprocess.run(['clang', '-O2', str(output), str(runtime), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
