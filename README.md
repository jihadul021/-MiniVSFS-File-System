# 🗂️ MiniVSFS - Mini Virtual Simple File System

**MiniVSFS** is a simple block-based inode file system implemented in **C** as part of the **CSE321: Operating Systems** course at **BRAC University**.  
It provides basic functionality for **filesystem image creation, file addition, and inode management** with support for both **direct and indirect pointers**.

---

## 📖 Features
- Block-based file system with inodes  
- Support for direct and indirect block pointers  
- Filesystem image creation utility  
- Add multiple files to the file system image  
- Inspect and debug file system content using `hexdump`  

---

## ⚙️ Usage

### 1️⃣ Compile Programs
Run the following commands to build the tools:
```
gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder
```
---

### 2️⃣ Build Filesystem Image
Create a file system image (`fs.img`) of size **512 KiB** with **256 inodes**:
```

./mkfs_builder --image fs.img --size-kib 512 --inodes 256
```
A new `fs.img` file will be generated.

---

### 3️⃣ Inspect Empty Filesystem
Check the empty space of the newly created filesystem:
```
hexdump -C fs.img | head -20
```

---

### 4️⃣ Add Files
You can add files to the filesystem image step by step:
```
./mkfs_adder --input fs.img --output fs1.img --file file_2.txt
./mkfs_adder --input fs1.img --output fs2.img --file file_18.txt
./mkfs_adder --input fs2.img --output fs3.img --file file_29.txt
./mkfs_adder --input fs3.img --output final_output.img --file file_31.txt
```


---

### 5️⃣ Verify Added Files
Use this command to check the final image and confirm that files were added successfully:
```
hexdump -C final_output.img
```
### Sample Output snapshot
<img width="770" height="860" alt="Screenshot from 2025-10-03 02-11-07" src="https://github.com/user-attachments/assets/14861ccd-a6aa-44fe-8698-e17109168ae8" />

