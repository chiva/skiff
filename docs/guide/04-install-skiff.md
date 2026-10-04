# 4. Install Skiff

## Download

1. Go to [Skiff releases](https://github.com/chiva/skiff/releases/latest).
2. Under **Assets**, download `skiff-<version>.zip`.

> [!TIP]
> Want to check the file is genuine and untouched? Every release zip is signed by the GitHub build
> that made it. With the [GitHub CLI](https://cli.github.com/) installed, run
> `gh attestation verify skiff-<version>.zip --repo chiva/skiff`. It should say the verification
> succeeded.

## Copy it to the PSP

1. Connect the PSP to your computer with the USB cable, then on the PSP choose Settings → USB
   Connection. (Or put the microSD card in a card reader.)
2. The Memory Stick appears on your computer as a drive.
3. Unzip `skiff-<version>.zip` **at the top level of that drive**. You should end up with:

   ```text
   PSP/
     GAME/
       Skiff/
         EBOOT.PBP
         LICENSE.txt
   ```

4. Eject the drive on your computer, then press ○ on the PSP to leave USB mode.

## Start it

Game → Memory Stick → **Skiff**.

✅ **Check:** Skiff's screen shows its version number.

## Update

Download the new zip and unzip it over the old one, replacing files when asked. Your settings and
downloaded games are kept.

## Uninstall

Delete the `PSP/GAME/Skiff` folder. Games you downloaded stay in `ISO/` until you delete them.

Next: [Connect to RomM](05-connect-to-romm.md)
