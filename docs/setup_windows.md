
1. open powershell
2. run .\tools\setup_win.ps1 for install dependence software packages
3. reopen powershell
4. create python env
  * python -m pip install venv
  * python -m venv venv
  * .\venv\Scripts\Activate.ps1
5. install west: pip install west
6. get source code:
  * west init -m <this_git_url> app
  * cd app; west update
6. install zephyr dependence python packages: 
  * cd app; pip install -r zephyr/scripts/requirements.txt

7. install zephyr sdk: west sdk install --version 0.16.9 -t arm-zephyr-eabi x86_64-zephyr-elf
8. build: west build -b daq_f407vet6 apps/applications/data_collect --sysbuil
