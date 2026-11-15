# Skins

A skin is the game's look and its drum sounds: a folder with `img/` and `snd/`, in the
format of the PC simulator Taikojiro. The packer puts one into the library
(`--skin`, see `docs/packer.md`).

| Folder | What it is |
|---|---|
| `taikojiro-original/` | **The default skin.** Taiko Pocket is developed and tested with it; it is also attached to the releases as a zip. |

To make your own, copy the folder, change the pictures and sounds, and pack with
`--skin your-folder`. A Taikojiro skin can be used as it is; `docs/packer.md` says
which of its pictures the game does not show.

## Credit

The default skin's pictures and drum sounds are by **toach**, the author of Taikojiro:
it is the "original" skin that comes with Taikojiro, with Taikojiro's drum sounds.
Taikojiro's readme says they were taken over from the earlier simulator Taiko-san
Taro. The skin is not covered by Taiko Pocket's GPL licence.
