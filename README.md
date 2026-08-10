# Flashing Instructions:
1) Extract repo zip file or pull down repo to local folder
2) Download VS-Code (or open it)
3) Install "PlatformIO IDE" VS-Code extension
4) Navigate to "PlatformIO Home" via the extension
5) Select "Open Project" and select the project folder
6) Wait for all the project dependencies and assorted files to finish downloading
7) Go to `View` > `Command Pallette` or use the shortcut `Ctrl + shft + p and type ` PlatformIO: Open Core CLI`, hit enter.
8) Ensure the badge is plugged in: hold the "Boot" button on the back of the badge as you plug in the  USB-C cable, the button can be released once the two green LEDs on the back of the board light up. Then run these commands IN ORDER
```
pio run --target erase
pio run --target clean
pio run --target upload
pio run --target uploadfs
```
 You should not receive any errors, after running all commands the badge is flashed.

NOTE: you may need to hit the "Reset" button on the back of the badge after flashing, If the LEDs don't immediately turn on after being flashed, hitting reset will fix it.

If you have any issues please open a GH issue or DM @Solaris on the [discord](https://discord.gg/thesafehouse)

# Badge CTF

Below are some hints, these aren't step by step instructions (that's no fun) but they are designed to give you a little inspiration if you are really stuck on something. I'm splitting up the hints in to two parts since the CTF has two distinct sections you must solve before you reach the end. Still stuck? come ask me about it on the [discord](https://discord.gg/thesafehouse) @Solaris

## Part 1: The Image
<details>
<summary>Hint</summary>
You should look up steganography :) 
</details>

<details>
<summary>Hint 2</summary>
Security by obscurity may be a helpful term to think about, just because you see something that you might not understand yet doesn't mean it wont be important later.
Take a hard look at the files within this repo.
</details>

## Part 2: Sleeper Activation
<details>
<summary>Hint</summary>
Good job, you got further than most.
Look deep in to the matrix and you might discover something.
</details>

<details>
<summary>Hint 2</summary>
Read up on number stations, they are really cool and still used today.
</details>
