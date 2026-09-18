import sys, base64
filename = sys.argv[1]
mode = sys.argv[2]
data = base64.b64decode(sys.argv[3]).decode('utf-8')
with open(filename, mode, encoding='utf-8') as f:
    f.write(data)
print(f'[{mode}] {filename}')
