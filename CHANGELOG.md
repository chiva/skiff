# Changelog

## [0.7.0](https://github.com/chiva/skiff/compare/v0.6.0...v0.7.0) (2026-10-14)


### Features

* **install:** install PSP games stored as CHD through RomM's conversion ([#119](https://github.com/chiva/skiff/issues/119)) ([a6df4b3](https://github.com/chiva/skiff/commit/a6df4b3e6c942d7f02cf7d21752b7c98618479c9))
* **jobs:** download files RomM converts ([#118](https://github.com/chiva/skiff/issues/118)) ([ef308a8](https://github.com/chiva/skiff/commit/ef308a88e7cd677f9789c7bd292d7f53e4a25886))
* **romm:** read RomM's download conversion support ([#116](https://github.com/chiva/skiff/issues/116)) ([b2ccb0c](https://github.com/chiva/skiff/commit/b2ccb0cb98706996e2cf38019b5361cabbf4aa54))


### Bug Fixes

* **app:** show RomM converting a download before its first bytes ([#121](https://github.com/chiva/skiff/issues/121)) ([aa45994](https://github.com/chiva/skiff/commit/aa459945cf49227b18dffc2dc31e4a1a1997702e))

## [0.6.0](https://github.com/chiva/skiff/compare/v0.5.0...v0.6.0) (2026-10-13)


### Features

* **app:** show a game's description and facts in its details ([#111](https://github.com/chiva/skiff/issues/111)) ([a6519d5](https://github.com/chiva/skiff/commit/a6519d5a90feb11ea2ffce68185e037e813fc10c))
* **details:** draw the game's name as a larger heading ([#114](https://github.com/chiva/skiff/issues/114)) ([a0c2275](https://github.com/chiva/skiff/commit/a0c227500a1ae9db8fa9c89a23d01ac6b2c22a02))

## [0.5.0](https://github.com/chiva/skiff/compare/v0.4.0...v0.5.0) (2026-10-12)


### Features

* **app:** add games sent from RomM ([#105](https://github.com/chiva/skiff/issues/105)) ([19c57a6](https://github.com/chiva/skiff/commit/19c57a68f116fb64baabf0811309b6dc1e6a5acb))
* **app:** check RomM for sent games every 10 s ([#108](https://github.com/chiva/skiff/issues/108)) ([c5dbee7](https://github.com/chiva/skiff/commit/c5dbee778ac3f75999ea54414a39b9ef1a427c5b))
* **romm:** RomM's install calls ([#103](https://github.com/chiva/skiff/issues/103)) ([dd958c1](https://github.com/chiva/skiff/commit/dd958c141fd47801b17f1e2b9bc38c57dd94147b))

## [0.4.0](https://github.com/chiva/skiff/compare/v0.3.0...v0.4.0) (2026-10-12)


### Features

* **app:** browse RomM collections ([#93](https://github.com/chiva/skiff/issues/93)) ([10150d6](https://github.com/chiva/skiff/commit/10150d6725e9c9de5589216e488d6f11eba695a5))
* **app:** clearer save sync screens ([#98](https://github.com/chiva/skiff/issues/98)) ([a3fc194](https://github.com/chiva/skiff/commit/a3fc194edd3ca2f1535b3ce244d0c7d7ec9d3b43))
* **app:** download all favourites ([#81](https://github.com/chiva/skiff/issues/81)) ([c8f6531](https://github.com/chiva/skiff/commit/c8f65315558e519408c58ddb1c992813fb570924))
* **app:** set Saves apart from the server rows in Settings ([#97](https://github.com/chiva/skiff/issues/97)) ([7c911ff](https://github.com/chiva/skiff/commit/7c911ffd6af87ab61e29f0903a81a9100c92c858))
* **app:** switch the library to favourites with SELECT ([#79](https://github.com/chiva/skiff/issues/79)) ([094fc11](https://github.com/chiva/skiff/commit/094fc11a73136aaaeee11b5878866a1a843a519a))
* **app:** sync saves ([#95](https://github.com/chiva/skiff/issues/95)) ([8f8213b](https://github.com/chiva/skiff/commit/8f8213bf8693ab8a48abcbe8a0e8dde907648b93))
* **jobs:** queue many downloads with one save ([#78](https://github.com/chiva/skiff/issues/78)) ([3b05917](https://github.com/chiva/skiff/commit/3b05917f0730d700c2bf058bf4b2130ab911bc32))
* **net:** stream request bodies from a reader ([#90](https://github.com/chiva/skiff/issues/90)) ([f6d3271](https://github.com/chiva/skiff/commit/f6d32710a82cf78409d4de1da5ffc65cad770ba1))
* **psp:** turn the screen off sooner while games download ([#85](https://github.com/chiva/skiff/issues/85)) ([83fa569](https://github.com/chiva/skiff/commit/83fa5692884ce89a1097e6ff3a14fce1cfc16bd1))
* **romm:** ask for save sync and collections when pairing, as optional scopes ([#88](https://github.com/chiva/skiff/issues/88)) ([2e2cd30](https://github.com/chiva/skiff/commit/2e2cd3091b81633a456576db8ed6c2581ea80e7c))
* **romm:** list a player's favourite games ([#77](https://github.com/chiva/skiff/issues/77)) ([6f40f29](https://github.com/chiva/skiff/commit/6f40f2930c84f36337ab05bc77b1fe7ce95e6b7e))
* **romm:** RomM's save and sync calls ([#91](https://github.com/chiva/skiff/issues/91)) ([b863187](https://github.com/chiva/skiff/commit/b863187337f8abde2d4e6a714cfd37e0ed6d0fc0))
* **romm:** test against RomM 5.4.0 and drop its newer-release notice ([#86](https://github.com/chiva/skiff/issues/86)) ([d1edcd7](https://github.com/chiva/skiff/commit/d1edcd78586c76aaf3a52508f960b1d8fd3bb213))
* **saves:** read, write and hash PSP save units ([#92](https://github.com/chiva/skiff/issues/92)) ([8cff119](https://github.com/chiva/skiff/commit/8cff119a73b80bf1405305a8b8f45d45b3c17f41))
* **saves:** restore save units safely, keep backups and a sync baseline ([#94](https://github.com/chiva/skiff/issues/94)) ([8c96dcc](https://github.com/chiva/skiff/commit/8c96dcc3c22275c85c3d2a24b122d6e3bd242b23))
* **storage:** list folders on the Memory Stick ([#89](https://github.com/chiva/skiff/issues/89)) ([dabaa10](https://github.com/chiva/skiff/commit/dabaa103e3278e806be99386543a1d3f2ff6cfcf))


### Bug Fixes

* **saves:** recover from sleep and closed sessions during save sync ([#99](https://github.com/chiva/skiff/issues/99)) ([6429256](https://github.com/chiva/skiff/commit/64292568c5c1df7c3d3f47bd25c642ce8e369870))


### Performance Improvements

* **saves:** faster hashing, restore and upload ([#100](https://github.com/chiva/skiff/issues/100)) ([e75cd1f](https://github.com/chiva/skiff/commit/e75cd1f85e7ee66dcb99bfe908e8354e070b456c))

## [0.3.0](https://github.com/chiva/skiff/compare/v0.2.0...v0.3.0) (2026-10-10)


### Features

* **app:** run RomM requests on their own thread, so the screen never waits ([#67](https://github.com/chiva/skiff/issues/67)) ([cefa8e2](https://github.com/chiva/skiff/commit/cefa8e2821f44cb8d0faf2c995d9d6c594b8cc6e))
* **app:** show a game's cover on its details screen ([#74](https://github.com/chiva/skiff/issues/74)) ([973647b](https://github.com/chiva/skiff/commit/973647b5ca0b762f4dbe4199f21284f5be99b6ed))
* **cover:** decode PNG covers into a picture the PSP can draw ([#71](https://github.com/chiva/skiff/issues/71)) ([38af384](https://github.com/chiva/skiff/commit/38af3841bb2e5f4dab7e3f5fd4be2f6b84b9df55))
* **cover:** keep decoded covers on the Memory Stick ([#73](https://github.com/chiva/skiff/issues/73)) ([bd31a7a](https://github.com/chiva/skiff/commit/bd31a7a82e1e1a8c248adbdef54f6e3fac8da71e))
* **romm:** read a game's cover path and build its address ([#70](https://github.com/chiva/skiff/issues/70)) ([ebb7e7f](https://github.com/chiva/skiff/commit/ebb7e7fc897ba35232f1942e201d8ad08e5d681a))


### Bug Fixes

* **ci:** give the emulator three tries to start, not two ([#63](https://github.com/chiva/skiff/issues/63)) ([8e6a854](https://github.com/chiva/skiff/commit/8e6a8542efd746562e80d15d1ad6c83c0dcef64a))
* **net:** bound name lookups on the PSP and the screen's requests ([#66](https://github.com/chiva/skiff/issues/66)) ([8a6869b](https://github.com/chiva/skiff/commit/8a6869b7b66acee3bac0abbb99f84541b9fc32b5))
* **ui:** lists fill the screen, download details sit under the list, and a round Circle ([#65](https://github.com/chiva/skiff/issues/65)) ([7f991bd](https://github.com/chiva/skiff/commit/7f991bdd22be9bf71ea40031194b17385e0dc759))

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
