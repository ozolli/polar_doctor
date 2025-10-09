#!/usr/bin/env python3
import sys
from PIL import Image
import os

# Chemins des images
img_path1 = "/home/ozolli/Programmation/polar_generator/Capture d'écran du 2025-10-08 11-33-09.png"
img_path2 = "/home/ozolli/Programmation/polar_generator/Capture d'écran du 2025-10-08 11-33-18.png"

# Lire la première image
print("=" * 80)
print("IMAGE 1: Capture d'écran du 2025-10-08 11-33-09.png")
print("=" * 80)
img1 = Image.open(img_path1)
print(f"Dimensions: {img1.size[0]}x{img1.size[1]} pixels")
print(f"Mode: {img1.mode}")
print(f"Format: {img1.format}")
print()

# Analyser les couleurs principales
colors1 = img1.getcolors(maxcolors=1000000)
if colors1:
    colors1_sorted = sorted(colors1, key=lambda x: x[0], reverse=True)[:10]
    print("Top 10 couleurs dominantes (count, RGB):")
    for count, color in colors1_sorted:
        print(f"  {count:8d} pixels - {color}")
print()

# Lire la deuxième image
print("=" * 80)
print("IMAGE 2: Capture d'écran du 2025-10-08 11-33-18.png")
print("=" * 80)
img2 = Image.open(img_path2)
print(f"Dimensions: {img2.size[0]}x{img2.size[1]} pixels")
print(f"Mode: {img2.mode}")
print(f"Format: {img2.format}")
print()

# Analyser les couleurs principales
colors2 = img2.getcolors(maxcolors=1000000)
if colors2:
    colors2_sorted = sorted(colors2, key=lambda x: x[0], reverse=True)[:10]
    print("Top 10 couleurs dominantes (count, RGB):")
    for count, color in colors2_sorted:
        print(f"  {count:8d} pixels - {color}")
print()

# Sauvegarder des versions temporaires avec noms simples pour Read tool
temp_path1 = "/home/ozolli/Programmation/polar_generator/temp_screenshot1.png"
temp_path2 = "/home/ozolli/Programmation/polar_generator/temp_screenshot2.png"

img1.save(temp_path1)
img2.save(temp_path2)

print(f"Images temporaires sauvegardées:")
print(f"  {temp_path1}")
print(f"  {temp_path2}")
