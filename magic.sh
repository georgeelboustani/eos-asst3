./configure
bmake
bmake install
cd kern/conf
./config ASST3
cd ../compile/ASST3
bmake depend
bmake
bmake install
cd ../../../

cd ../root
if [ "$1" =  "-d" ]
then
  sys161 -w kernel q
else
  sys161 kernel q
fi
cd ..
