# Reference build hashes

SHA-256 hashes of the builds this project targets. Before you share offsets, addresses or struct layouts, check that your files match these. Files marked **★** are primary reverse-engineering targets.

> **Launchers differ between distributions.** `AVP2.exe` and `PrimalHunt.exe` are front-end launchers, not the engine. In the reference install, both appear to have been modified by the repack they came from: `AVP2.exe` is packed, and `PrimalHunt.exe` has a 2025 link timestamp. Don't treat them as reference binaries, and don't target them. The engine is started through `lithtech.exe`.

## Aliens versus Predator 2: English 1.0.9.6 + SP Map Update #1 + MP Map Update #1 (install folder)

| File | Size (bytes) | SHA-256 |
|---|---:|---|
| `ALIEN.REZ` | 152,647,526 | `7d23e9ef9c79d9b5a1ea32b09b742b410e5605fc0d814f0e3fe827cc4f502533` |
| `AVP2.REZ` | 529,599,976 | `2f7a99daa71929bc3f64ce5f58c6be0ca07e17d4fa024d58c9826e241606010c` |
| `AVP2.exe` (launcher, varies) | 1,593,344 | `e9e92aeb21bc1a899b31dcffb56afa5f87b502e08311fce76d96a65e410310a3` |
| `AVP2DLL.REZ` | 5,284,080 | `62b7ad0659fbfb5832ad369174be8c9fa66d1acfd7941fb80f69b5d34e8f4c9d` |
| `AVP2L.REZ` | 2,961,956 | `5d1963fee979c213f67bc4724e4dfdfa235d87fdadde326f9ae97ec6dcdffc79` |
| `AVP2P.REZ` | 45,287,360 | `4f3700985a99e99fe1d44bfe98b7cb3bc530cb02d15a5bd3ea959deba2df9ab9` |
| `AVP2P1.REZ` | 4,659,497 | `3a5b8dafb7b6a39ff06fa661ab2dbc1255aca06b4e0a05f0b53db89f1376f6c1` |
| `AVP2SP.REZ` | 46,730,029 | `81c9f2225029a1cf5cbc5530c1bb4c77aa8f284132a61e1d9751166966d07116` |
| `AVP2Serv.exe` | 1,282,048 | `42431431a0131b7c24e69b2e346086d1ec3e9696ba15ec0d51080466663a982b` |
| `AVP2Up.dll` | 249,856 | `95bca7399e740b00023a2f43c64da55ca9621aac9fc6204e1c3422a775f7bb51` |
| `Autorun.exe` | 45,056 | `d1ad76f37586f37a77bce6f3acf6976e46c85fb94ef482b0261583f7db3d568d` |
| `Launch.dll` | 241,722 | `0a66417229cfbc673f31a08603dbdcf53a0a4725a75ae3f001e68b468da66c89` |
| `MARINE.REZ` | 148,540,645 | `8f812962fb497e1cac6e1cf84878403c7ada6beaf5bc9dea22f7a360ba5012ae` |
| `MULTI.REZ` | 52,722,873 | `6564107f721c4ca4d4120a4668956ee17a3fd74e9d6a124be574b66ff6698878` |
| `PREDATOR.REZ` | 110,196,968 | `a50fd34ee7f269b5a08c897ea980d7724acfa66353d5cda7c8c1ed3b3a43b88d` |
| `SOUNDS.REZ` | 198,356,809 | `fe4619fdaf786306fbaefba113f503bf65d860f367137a87dfe87f82d44a2531` |
| `SierraPt.dll` | 606,208 | `7906f305052e4f0d9c5dc8039c9e880edcc1b08e007498f7b06e4323461f5e09` |
| `SierraUp.exe` | 462,848 | `6e31358c3f1f23b5520201004cf3542a0db00061686e8d50105d8e0628a46588` |
| `Smackw32.dll` | 83,456 | `ab21e93c78f8ffdd88585b5bcf71d846eb11ca72f274e17beac240bcc3334438` |
| `binkw32.dll` | 262,656 | `6b33d3f6a522f08be15bf57a9af39aa6bcaaea3233e37b8dd66447a8f8871b59` |
| `d3d.ren` ★ | 348,160 | `108d4e9083cffa97dbe9ae8d7d1c38f1179b9d7d627a11c0cc6483a48349226c` |
| `lithtech.exe` ★ | 937,984 | `62c8f98091187909df0edfb062b0ed4abd09c7c4af75991c0a2bae713087b0be` |
| `ltmsg.dll` | 32,768 | `8552415b8818018b23561b03fddc3550adea136e621123b9dcdec5bb0c2b6fa6` |
| `mss32.dll` | 347,648 | `f09e6024ad35eb6b7b46982efb998df26d162391b26fcf56519a4c23fcbc7015` |
| `server.dll` ★ | 557,056 | `ee6b51d6075791c7aa8bd2af3a1f4317052d4899567ecaea6807701c0af7e2d1` |

## AVP2 1.0.9.6: modules inside REZ archives (extract with `tools/rez.py`)

| File | Size (bytes) | SHA-256 |
|---|---:|---|
| `AVP2DLL/cshell.DLL` ★ | 2,084,864 | `e4f8fa8e1d0e8f8ba49c3433fbccbb32077255b77b8f5182702de493227c2146` |
| `AVP2DLL/object.LTO` ★ | 3,198,976 | `bad979b96acefea404de1cfae87a342dce8f31171bf5af976c67d3fd649da16c` |
| `AVP2L/cres.DLL` ★ | 446,464 | `f40118f70ff87b8dfdfe3d30d57beac7538139c0af85194013ed1300a208a365` |
| `AVP2L/sres.DLL` ★ | 45,056 | `0283a36c78cd121701a93c7f404ebafc0e75bd2d2e94dcbbb8e6b764264f4ade` |

## Primal Hunt: retail install (separate folder)

| File | Size (bytes) | SHA-256 |
|---|---:|---|
| `AVP2DLL.REZ` | 5,558,512 | `52a0393219f836c2ededfc3f8c0e8e8478cc726d7febc73409a825a070f3791e` |
| `AVP2L.REZ` | 3,119,634 | `8d4845771bebefb2418876f5096e3cb70afd08f5d5159db370140b53732c7c40` |
| `AVP2P.REZ` | 45,287,360 | `4f3700985a99e99fe1d44bfe98b7cb3bc530cb02d15a5bd3ea959deba2df9ab9` |
| `AVP2P1.REZ` | 4,659,497 | `3a5b8dafb7b6a39ff06fa661ab2dbc1255aca06b4e0a05f0b53db89f1376f6c1` |
| `AVP2X.REZ` | 291,009,005 | `20c81a7fdf717ed03020bb5be63dd008c235cf00a8551072ce7972bb89c526eb` |
| `AVP2XServ.exe` | 1,298,432 | `c9ced4219bce07c6e9258339321da87f8a053d56fd09fcb4e905dfe258198b57` |
| `AVP2XUp.dll` | 634,880 | `48eb06938b653ca9c7ee935c3c9ad50daa60ea03979820ef1091f163b3ec260a` |
| `DIALOGUE.REZ` | 6,657,455 | `e45a6897992b90053c0b88127eab6d1680c779ed51d4d4b52ec16ce1f46cf9b5` |
| `Launch.dll` | 241,722 | `0a66417229cfbc673f31a08603dbdcf53a0a4725a75ae3f001e68b468da66c89` |
| `MULTI.REZ` | 52,722,873 | `6564107f721c4ca4d4120a4668956ee17a3fd74e9d6a124be574b66ff6698878` |
| `PrimalHunt.exe` (launcher, varies) | 7,216,595 | `90f90952c95803e985470e2264608a8017e75e8887500537422559b1593b25fa` |
| `SierraPt.dll` | 630,784 | `b5dba72784012542edbfabc4f0805d2e01453dfc0f9c4b77cebc5022bdad2b22` |
| `SierraUp.exe` | 466,944 | `fdacea5607f5da00b9ea0f34cc4c91bab7f3e06a09eb5739670bbda503017d7f` |
| `Smackw32.dll` | 83,456 | `ab21e93c78f8ffdd88585b5bcf71d846eb11ca72f274e17beac240bcc3334438` |
| `SuDE.dll` | 57,344 | `2c6a1f2187c3e5ebc03ec642697fda518b9d33f8a97d5022717614677301a52e` |
| `SuES.dll` | 57,344 | `3202c2389857709c973669716b2b7169d38b029cc5f5b090486bce7f8b50ddef` |
| `SuFR.dll` | 57,344 | `7dacf6e7ee35773c84197d7db7d2ffd04eca60650ad4d2d4a7f671e036be7343` |
| `SuIT.dll` | 57,344 | `908472b355029d669f127c964d68592edc40e4dc86deec300505a508e1817c35` |
| `binkw32.dll` | 262,656 | `6b33d3f6a522f08be15bf57a9af39aa6bcaaea3233e37b8dd66447a8f8871b59` |
| `cdaudio.dll` | 102,400 | `f5f374161dee45ed903eda896dae58ba8b7fc1b1f20225aec7f174c04606af83` |
| `d3d.ren` ★ | 364,544 | `43b2397164cdfd49b87c3d4af8c17755e13ba48f58fb6fdec98f9d53616109da` |
| `lithtech.exe` ★ | 1,007,616 | `042cf0d6f1fa5c4c29b662b43930690dfa34067d315392f7cd76e9bb1b6b2f89` |
| `ltmsg.dll` | 45,056 | `5fcb7801c9142870a68a709c89aef9866853abdc9850b46622c2b75e633edefe` |
| `mss32.dll` | 347,648 | `f09e6024ad35eb6b7b46982efb998df26d162391b26fcf56519a4c23fcbc7015` |
| `server.dll` ★ | 610,304 | `2f53d30d4223ab7e7a006b490de2620953af8194145810cec9fcd932a2bdf268` |

## Primal Hunt: modules inside REZ archives

| File | Size (bytes) | SHA-256 |
|---|---:|---|
| `AVP2DLL/CSHELL.DLL` ★ | 2,183,168 | `c2d0a0e06560b84f6d57d9eadf814d4628a48e7ac0333c26cbe89e88463e52e4` |
| `AVP2DLL/OBJECT.LTO` ★ | 3,375,104 | `70ec969758fecde4832cc3acd4b31f0f8e2fd9edb7231a7ba5658b02c121d75a` |
| `AVP2L/CRES.DLL` ★ | 503,808 | `ad33827c0ce63a943c50076a7779f266cfbfad38d315d6497948957385022165` |
| `AVP2L/SRES.DLL` ★ | 45,056 | `b919adb5a9eaf83c39afca68874736a9bddb7fc30b790fc283d0c9d443996734` |

## Verifying your copy

```sh
# Linux / macOS / WSL / Git Bash
sha256sum lithtech.exe d3d.ren server.dll
# PowerShell
Get-FileHash lithtech.exe, d3d.ren, server.dll -Algorithm SHA256
```
