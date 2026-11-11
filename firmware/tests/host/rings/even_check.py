"""0.4's 44.1 kHz voice against the 88.2 kHz build (Makefile `voice44`): A's even samples must equal B's, and B's odd
samples must repeat its even ones (the core's 2x). Both files are int16 blocks of 128. Python 3.8, stdlib only."""
import array
import sys


def load(path):
    a = array.array('h')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    return a


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    if len(a) != len(b):
        print('voice44: lengths differ: %d vs %d samples' % (len(a), len(b)))
        return 1
    even = sum(1 for i in range(0, len(a), 2) if a[i] != b[i])
    rep = sum(1 for i in range(0, len(b), 2) if b[i] != b[i + 1])
    nonzero = sum(1 for i in range(0, len(b), 2) if b[i])
    print('voice44: %d samples at 44.1 kHz, %d non-zero, %d differ from the 88.2 kHz build (even samples), '
          '%d not repeated' % (len(b) // 2, nonzero, even, rep))
    return 0 if even == 0 and rep == 0 and nonzero else 1


if __name__ == '__main__':
    sys.exit(main())
