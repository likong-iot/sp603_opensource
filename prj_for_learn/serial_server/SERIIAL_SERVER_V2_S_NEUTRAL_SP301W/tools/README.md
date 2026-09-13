<!--
 * @Author: Orion
 * @Date: 2024-08-13 14:08:55
 * @LastEditors: Orion
 * @LastEditTime: 2024-08-30 17:29:03
 * @Description: 
 * 
-->
# RUN

python -m pip install pyside6 -i https://pypi.tuna.tsinghua.edu.cn/simple

#  Pyinstaller
``` bash
pyinstaller -F -w -i logo.ico --add-binary "libs/TSCLIB.dll;." main.py
pyinstaller -F -w -i logo.ico --add-binary "libs/TSCLIB.dll;." --onefile --add-data "images/*;images" main.py
pyinstaller -F -w -i lklogo.ico --exclude-module PyQt5  --onefile --add-data "images/*;images" main.py
```

# Install

``` bash
pip install pyqt5 -i https://pypi.tuna.tsinghua.edu.cn/simple
pip install pyqt5-tools -i https://pypi.tuna.tsinghua.edu.cn/simple
pip install qt_material -i https://pypi.tuna.tsinghua.edu.cn/simple
```
# Run

``` bash
python main.py
```

# UI

``` bash
pyuic5 scan_device.ui -o scan_device_ui.py

```