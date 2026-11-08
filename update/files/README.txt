Files under update/files mirror /disk on RadiumOS and are shipped by `make update-package`.

  update/files/apps/notes.hls   ->  /disk/apps/notes.hls
  update/files/.radiumrc        ->  /disk/.radiumrc   (only if the user has none yet)

Workflow (no ISO rebuild, no reflashing):
  make update-disk              build os.bin and write the package to fetchdisk.img as /disk/update/
  (boot RadiumOS)  update apply  then  reboot
  make serve-update             or serve dist/update over HTTP: update apply http://10.0.2.2:8000/
                                (HTTP is limited to 16 KB per file, so the kernel itself goes via update-disk)
