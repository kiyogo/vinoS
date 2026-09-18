import sys
args = sys.argv[1]
open(args, 'w', encoding='utf-8').write(sys.stdin.read())
print('Wrote ' + args)
