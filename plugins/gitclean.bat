rem git clean -xfdf
git submodule foreach --recursive git clean -xfdf
rem git reset --hard
git submodule foreach --recursive git reset --hard
git submodule update --init --recursive