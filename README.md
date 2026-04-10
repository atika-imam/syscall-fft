# syscall-fft
### Part 1: Windows Setup (The Host)
1. Install the Tools
  Download VS Code: Go to code.visualstudio.com and download the Windows installer. Run it and finish the setup.
  
  Install MinGW (The Compiler): * Go to winlibs.com or download the "MinGW-w64" installer.
  Crucial: Once installed, you must add the bin folder to your Environment Variables (Path) so Windows knows where g++ is. (Search "Edit the system environment variables" in your Start menu).
  
  Setup VS Code:
  Open VS Code.
  Press Ctrl+Shift+X, search for "C++", and install the one by Microsoft.

2. Test "Hello World"
  Open VS Code, click File > New Text File, and save it as hello.cpp.
  
  Type: #include <iostream> 
  int main() { 
    cout << "Hello World"; 
    return 0; 
  }.
  
  Run file, if it prints "Hello World," your Windows is ready.

### 3. Run the Final Windows Project
Create a folder on your Desktop named OS_Project.
Open this folder in VS Code.
Create a new file named syscall.cpp.
Paste the Windows Code .

Run it: In the VS Code terminal.
The terminal will print: Windows Execution Completed in: 0.XXXX s.

### Part 2: Kali Linux Setup (The VM)
1. Open Terminal and Install Compiler
Start your Kali VM and log in.
Click the Terminal icon (top left).
Type: sudo apt update && sudo apt install build-essential -y
Enter your password (you won't see characters as you type).

3. Create the Project File
In the terminal, create a folder: mkdir OS_Project && cd OS_Project
Create the file using the "Nano" editor: nano syscall.c
Paste the Linux Code.
Save and Exit: * Press Ctrl + O (to write out).
Press Enter (to confirm the name).
Press Ctrl + X (to exit the editor).

3. Run the Final Linux Project
In the terminal, compile the code:
gcc syscall.c -o syscall -lm

Run the code:
./syscall
The terminal will print Process_Started.
A file named output.txt will be created in your folder.
The terminal will print: Linux Execution Completed in: 0.XXXX s.
