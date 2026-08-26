import urllib.request
import io
import traceback

textures = {
    'wool': 'wool_colored_silver.png',
    'planks_oak': 'planks_oak.png',
    'obsidian': 'obsidian.png',
    'end_stone': 'end_stone.png',
    'hardened_clay': 'hardened_clay_stained_white.png',
    'glass': 'glass.png'
}

base_url = "https://raw.githubusercontent.com/InventivetalentDev/minecraft-assets/1.8.8/assets/minecraft/textures/blocks/"

headers = {'User-Agent': 'Mozilla/5.0'}

with open('Mc_Injector-master/agent/block_textures.h', 'w') as f:
    f.write('#pragma once\n\n')
    for name, file in textures.items():
        url = base_url + file
        try:
            req = urllib.request.Request(url, headers=headers)
            resp = urllib.request.urlopen(req)
            data = resp.read()
            f.write(f'static const unsigned char BLOCK_{name.upper()}_PNG[] = {{\n')
            f.write(','.join(str(b) for b in data))
            f.write('\n};\n\n')
            print(f'Downloaded {name}')
        except Exception as e:
            print(f'Failed {name}: {e}')
