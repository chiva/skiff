# Changelog

## [0.2.0](https://github.com/chiva/skiff/compare/v0.1.0...v0.2.0) (2026-10-09)


### Features

* **app:** run the app on the PSP ([#54](https://github.com/chiva/skiff/issues/54)) ([997fad3](https://github.com/chiva/skiff/commit/997fad3d3a86d75c9283d07f8570a83989581d63))
* **app:** screens and state machine, host-tested ([#52](https://github.com/chiva/skiff/issues/52)) ([897adae](https://github.com/chiva/skiff/commit/897adaebe1d1a29b26ffc295df9fffe7212bdfaf))
* **app:** show the pairing address as a QR code ([#58](https://github.com/chiva/skiff/issues/58)) ([0a36e32](https://github.com/chiva/skiff/commit/0a36e32ab217ab9acd61f0ac50f26f03eaf6dd82))
* **config:** choose the log level with [log] level ([#33](https://github.com/chiva/skiff/issues/33)) ([6744938](https://github.com/chiva/skiff/commit/6744938242dc3598465a3d0bb261454aedf45f80))
* **config:** read and write config.ini ([#32](https://github.com/chiva/skiff/issues/32)) ([2597a33](https://github.com/chiva/skiff/commit/2597a33175f05019d9fcd157c1308386819afec6))
* **install:** installed.json manifest and the PSP installer ([#42](https://github.com/chiva/skiff/issues/42)) ([967cd9e](https://github.com/chiva/skiff/commit/967cd9e523daa96890e436a9486b106db7b6f305))
* **jobs:** hand a finished download to the installer before the queue says done ([#50](https://github.com/chiva/skiff/issues/50)) ([6c8fc61](https://github.com/chiva/skiff/commit/6c8fc6122410235d832532891d06082ec54e7423))
* **jobs:** persistent download queue and retry policy ([#40](https://github.com/chiva/skiff/issues/40)) ([7121605](https://github.com/chiva/skiff/commit/71216059bd7d9c0fe727ca38136db7016ef8c536))
* **jobs:** resume downloads from a .part file with a CRC-32 checkpoint ([#25](https://github.com/chiva/skiff/issues/25)) ([2a0cd16](https://github.com/chiva/skiff/commit/2a0cd161db40e015f97ade5b0f0c813c0533064e))
* **jobs:** run the download queue on a worker thread on the PSP ([#41](https://github.com/chiva/skiff/issues/41)) ([2a1ec5e](https://github.com/chiva/skiff/commit/2a1ec5e5443d29d5eaca15353eec8c6fa144c45a))
* **log:** skiff.log with levels, rotation and redaction ([#31](https://github.com/chiva/skiff/issues/31)) ([49dc44d](https://github.com/chiva/skiff/commit/49dc44da22b41ba8e77b7013499bc24f91cf4192))
* **net:** let the caller stop a transfer ([#24](https://github.com/chiva/skiff/issues/24)) ([0e6ac35](https://github.com/chiva/skiff/commit/0e6ac35eb29d25e91378527bd96bd59f4443eb79))
* **psp:** keyboard, network picker and a join that does not block the UI ([#51](https://github.com/chiva/skiff/issues/51)) ([d45d715](https://github.com/chiva/skiff/commit/d45d715ea5b1a65dd5ec1da4c52a0a6da913f55c))
* **romm:** heartbeat, version policy and paginated platforms and ROMs ([#34](https://github.com/chiva/skiff/issues/34)) ([720914b](https://github.com/chiva/skiff/commit/720914bdf7a85dcdb2112f60778bb6afb54d5da6))
* **romm:** pair with RomM through the device-code flow ([#39](https://github.com/chiva/skiff/issues/39)) ([78ccdd2](https://github.com/chiva/skiff/commit/78ccdd22163ce8cbb9809de35f534ddb35b3ffff))
* **storage:** logical roots, folders, free space and safe names ([#35](https://github.com/chiva/skiff/issues/35)) ([5ac804e](https://github.com/chiva/skiff/commit/5ac804ee21d29e10f57ebbf1ed982d3217a1ab44))
* **ui:** list and progress models, GU widgets, and English and Spanish text ([#38](https://github.com/chiva/skiff/issues/38)) ([6f35cf8](https://github.com/chiva/skiff/commit/6f35cf89ff528c2c4ea29ce704e05f2de2f4e15b))


### Bug Fixes

* **app:** a clean starting screen, Wi-Fi joins retried, and no Start-to-quit ([#60](https://github.com/chiva/skiff/issues/60)) ([2eaca7b](https://github.com/chiva/skiff/commit/2eaca7bbb631091f3f0ee462d4b5cdc349a4b8d4))
* **app:** a tidier pair screen, and no 2.6 s freeze on the first free-space query ([#59](https://github.com/chiva/skiff/issues/59)) ([a400191](https://github.com/chiva/skiff/commit/a4001918b1c862f68ced02de0018955d483f9b7d))
* **app:** show RomM's pairing address with the code in it, and act on the rest of A1 ([#57](https://github.com/chiva/skiff/issues/57)) ([9934558](https://github.com/chiva/skiff/commit/99345581430e91ec65340563d02090c28fa75bc8))
* **ci:** give the UI prototype 90 s in the emulator, not 30 ([#43](https://github.com/chiva/skiff/issues/43)) ([5d08407](https://github.com/chiva/skiff/commit/5d08407f3ae8461d41ba646aa6922a5d6431db52))
* **jobs:** save the queue file without holding the lock the UI reads ([#46](https://github.com/chiva/skiff/issues/46)) ([a092a79](https://github.com/chiva/skiff/commit/a092a79b53101668aa5da67ac78a2698b0dd7941))
* **log:** keep refused lines and write them with the next batch ([#47](https://github.com/chiva/skiff/issues/47)) ([fc2d954](https://github.com/chiva/skiff/commit/fc2d9547dd1b6b669f1bad98cef3e04f71e83e18))
* **memstick:** read the seeded payload's fields, not the nested extra file's ([#45](https://github.com/chiva/skiff/issues/45)) ([1a2c177](https://github.com/chiva/skiff/commit/1a2c177790b2a354edcba590d392dd2328320db4))
* **romm:** show a file with an unusable name instead of refusing its page ([#36](https://github.com/chiva/skiff/issues/36)) ([9103287](https://github.com/chiva/skiff/commit/9103287ddf149b3aa4358bb468e8ba4485b1b66c))
* **test:** remove the macOS ._ files left beside the app's CA bundle and config.ini ([#61](https://github.com/chiva/skiff/issues/61)) ([df9291e](https://github.com/chiva/skiff/commit/df9291ea99285c78c9fdd2d62a9f50354c3fc28b))


### Performance Improvements

* **jobs:** write downloads to the Memory Stick in 1 MiB blocks ([#30](https://github.com/chiva/skiff/issues/30)) ([86c61b4](https://github.com/chiva/skiff/commit/86c61b4ff42df62d97d2881d70b2991e1e63cfde))

## 0.1.0 (2026-10-05)


### Features

* give Skiff an XMB icon, background and GitHub artwork ([#8](https://github.com/chiva/skiff/issues/8)) ([f9e8679](https://github.com/chiva/skiff/commit/f9e8679911d82afc38ca6a156edac68e30131cb0))
* **net:** add the PSP network layer and a network probe for the hardware spike ([#16](https://github.com/chiva/skiff/issues/16)) ([148a6a0](https://github.com/chiva/skiff/commit/148a6a07823a82026226d0e13007c0f287f22b5f))
* **net:** add the transport layer, curl/TLS error mapping and a fake transport ([#18](https://github.com/chiva/skiff/issues/18)) ([f2b5014](https://github.com/chiva/skiff/commit/f2b50145c5cf00f33019ec4bb532cd145aa6c78f))
* **net:** run at 333 MHz while online and pin ChaCha20-first ciphers ([#23](https://github.com/chiva/skiff/issues/23)) ([ba92213](https://github.com/chiva/skiff/commit/ba9221344e6d5e0bdc06ed13e74ca9fca9fe1866))
* **net:** seed TLS from KIRK through ARK, behind a health test ([#13](https://github.com/chiva/skiff/issues/13)) ([b5cfa2e](https://github.com/chiva/skiff/commit/b5cfa2e53f21e621eefcb390be67932c80336d2b))
* **test:** add a disposable RomM behind a TLS proxy for integration and network tests ([#15](https://github.com/chiva/skiff/issues/15)) ([be872bc](https://github.com/chiva/skiff/commit/be872bcff494a5a66ec73bd11cfa08cad6b98b48))
* **test:** add a GU + intraFont UI prototype for the hardware spike ([#14](https://github.com/chiva/skiff/issues/14)) ([fe37873](https://github.com/chiva/skiff/commit/fe37873136d18509d1332076ee6fe5716e43b649))
* **test:** add a KIRK random generator probe for the hardware spike ([#12](https://github.com/chiva/skiff/issues/12)) ([fba79ef](https://github.com/chiva/skiff/commit/fba79ef188c768bf49402537cf4690cd617c9b30))
* **toolchain:** build curl 8.22 and Mbed TLS 4.1 LTS into a Skiff toolchain image ([#2](https://github.com/chiva/skiff/issues/2)) ([6cfa23b](https://github.com/chiva/skiff/commit/6cfa23b63c4b0821a5c1265d352e53cd61e5f1cd))


### Bug Fixes

* **bench:** reject plain_http values other than 0 or 1 ([#22](https://github.com/chiva/skiff/issues/22)) ([c725170](https://github.com/chiva/skiff/commit/c725170ddbecdad960aad9ec059e3fbc66760dfc))
* **ci:** retry the emulator once when it dies before loading the EBOOT ([#5](https://github.com/chiva/skiff/issues/5)) ([85eb56a](https://github.com/chiva/skiff/commit/85eb56a6548c7ea654d8a3cad11a10ef805e29f1))
* **release:** start versioning at 0.1.0 instead of 1.0.0 ([#4](https://github.com/chiva/skiff/issues/4)) ([c4e9040](https://github.com/chiva/skiff/commit/c4e9040122a396ff22b59e5d0bcc589732143ea8))
* **test:** remove the macOS ._ files memstick.sh leaves beside Skiff's folders ([#11](https://github.com/chiva/skiff/issues/11)) ([618a1b3](https://github.com/chiva/skiff/commit/618a1b309d821f53e198dfeb85060bda343805f7))
* **toolchain:** hermetic image build and fail-closed mbedtls profile ([#9](https://github.com/chiva/skiff/issues/9)) ([fd22518](https://github.com/chiva/skiff/commit/fd22518fe9608d1701a4a0c57af28c4ca0b6f338))
