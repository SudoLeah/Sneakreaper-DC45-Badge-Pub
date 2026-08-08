# Flashing Instructions:
1) extract repo zip file or pull down repo to local folder
2) download VS-Code (or open it)
3) Install "PlatformIO IDE" vscode extension
4) Navigate to "PlatformIO Home" via the extension
5) Select "Open Project" and select the project folder
6) Wait for all the project dependanices and assorted files to finish downloading
7) Go to `View` > `Command Pallette` or use the shortcut (~Ctrl + shft + p~) and type `PlatformIO: Open Core CLI`, hit enter.
8) Ensure the badge is plugged in: hold the "Boot" button on the back of the badge as you plug in the  USB-C cable, the button can be released once the two green LEDs on the back of the board light up. Then run these commands IN ORDER
```
pio run --target erase
pio run --target clean
pio run --target upload
pio run --target uploadfs
```
 You shoukd not receive any errors, after running all commands the badge is flashed.

NOTE: you may need to hit the "Reset" button on the back of the badge after flashing, If the LEDs don't immedaitly turn on after being flashed, hitting reset will fix it.

If you have any issues please open a GH issue or DM @Solaris on the [discord](https://discord.gg/thesafehouse)

# Badge CTF

More info to come soon, Once the defcon chaos has settled down i'll get to writing some hints and helpful tips for this, it's fully functional as-is so feel free to get cracking now :) But if you need a little extra help check back here in a day or two for some tips.

