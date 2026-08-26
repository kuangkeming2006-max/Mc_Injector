import urllib.request
from PIL import Image
import io

url = "https://minecraft.wiki/images/Red_Bed_%28item%29_JE3_BE2.png"
req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
response = urllib.request.urlopen(req)
img_data = response.read()

img = Image.open(io.BytesIO(img_data)).convert("RGBA")
img = img.resize((16, 16), Image.NEAREST)

with open("Mc_Injector-master/agent/bed_texture.h", "w") as f:
    f.write("#pragma once\n\n")
    f.write("static const unsigned char BED_TEXTURE_RGBA[16 * 16 * 4] = {\n")
    pixels = list(img.getdata())
    for i, p in enumerate(pixels):
        f.write(f"{p[0]},{p[1]},{p[2]},{p[3]},")
        if (i+1) % 16 == 0:
            f.write("\n")
    f.write("};\n")
