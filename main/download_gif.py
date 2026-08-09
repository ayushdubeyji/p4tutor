import urllib.request, os

# Download a small robot/person animated GIF
url = 'https://media.giphy.com/media/3o7btQ0NH6Kl8CxCfS/giphy.gif'
gif_path = 'teacher.gif'

try:
    urllib.request.urlretrieve(url, gif_path)
    print("Downloaded GIF.")
except Exception as e:
    print(f"Failed to download from primary URL: {e}")
    try:
        url = 'https://upload.wikimedia.org/wikipedia/commons/thumb/2/2c/Rotating_earth_%28large%29.gif/200px-Rotating_earth_%28large%29.gif'
        urllib.request.urlretrieve(url, gif_path)
        print("Downloaded fallback GIF.")
    except Exception as e2:
        print(f"Failed to download from fallback URL: {e2}")

if os.path.exists(gif_path):
    with open(gif_path, 'rb') as f:
        data = f.read()
    with open('teacher_gif.h', 'w') as out:
        out.write('#pragma once\n#include <stdint.h>\n')
        out.write(f'static const uint8_t teacher_gif_data[] = {{\n')
        for i, b in enumerate(data):
            if i % 16 == 0: out.write('    ')
            out.write(f'0x{b:02X}, ')
            if (i+1) % 16 == 0: out.write('\n')
        out.write('\n};\n')
        out.write(f'static const uint32_t teacher_gif_size = {len(data)};\n')
    print(f'Converted {len(data)} bytes')
else:
    print("No GIF found to convert.")
