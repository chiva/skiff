# Changelog

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
