
1. open powershell
2. run .\tools\setup_win.ps1 for install dependence software packages
3. add "C:\Program Files\7-Zip" to PATH and reopen a powershell
4. install python and create python env
  * uv python install 3.12.8
  * uv venv venv -p 3.12.8
  * .\venv\Scripts\activate.ps1
5. install west: uv pip install west
6. get source code:
  * west init -m <this_git_url> app
  * cd app; west update
6. install zephyr dependence python packages: 
  * cd app; uv pip install -r zephyr/scripts/requirements.txt

7. install zephyr sdk: west sdk install --version 0.16.9 -t arm-zephyr-eabi x86_64-zephyr-elf
8. build: west build -b daq_f407vet6 apps/applications/data_collect --sysbuild
