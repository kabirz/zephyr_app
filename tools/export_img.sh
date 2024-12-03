#/bin/bash

./apps/tools/gen_app.sh
mkdir images
cp build/app.bin build/data_collect/zephyr/zephyr.signed.bin images
cd apps/tools || exit
cp parser_raw.py smp_upload.py udp_multi_getinfo.py udp_multi_setinfo.py ../../images
cd ../.. || exit
zip images.zip -r images
rm -fr images
