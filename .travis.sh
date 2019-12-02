#/bin/bash

travis_before_install() 
{
    if [ "$TARGET_OS" = "Linux" ]; then
        sudo add-apt-repository --yes ppa:ubuntu-toolchain-r/test
        if [ "$TARGET_ARCH" = "ARM64" ]; then
            sudo apt update -qq
            sudo apt install -y gcc-9 g++-9 qtbase5-dev libcurl4-openssl-dev libgl1-mesa-dev libglu1-mesa-dev libalut-dev libevdev-dev libgles2-mesa-dev
        else
            wget -c "https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage"
            chmod a+x linuxdeployqt*.AppImage
            wget -c "https://github.com/RPCS3/AppImageKit-checkrt/releases/download/continuous2/AppRun-patched-x86_64" -O AppRun
            chmod a+x AppRun
            wget -c "https://github.com/RPCS3/AppImageKit-checkrt/releases/download/continuous2/exec-x86_64.so" -O exec.so

            sudo add-apt-repository --yes ppa:beineri/opt-qt-5.12.3-xenial
            sudo apt update -qq
            sudo apt install -qq qt512base gcc-9 g++-9 libgl1-mesa-dev libglu1-mesa-dev libalut-dev libevdev-dev
        fi
    elif [ "$TARGET_OS" = "Linux_Clang_Format" ]; then
        wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -
        sudo apt-add-repository --yes "deb http://apt.llvm.org/trusty/ llvm-toolchain-trusty-6.0 main"
        sudo add-apt-repository --yes ppa:ubuntu-toolchain-r/test        
        sudo apt-get update -y
        sudo apt-get install -y clang-format-6.0
    elif [ "$TARGET_OS" = "OSX" ]; then
        brew update
        brew install qt5
        npm install -g appdmg
        curl -L --show-error --output vulkansdk.tar.gz https://vulkan.lunarg.com/sdk/download/${VULKAN_SDK_VERSION}/mac/vulkansdk-macos-${VULKAN_SDK_VERSION}.tar.gz?Human=true
        tar -zxf vulkansdk.tar.gz
    elif [ "$TARGET_OS" = "IOS" ]; then
        brew update
        brew install dpkg
    elif [ "$TARGET_OS" = "Android" ]; then
        echo y | sdkmanager 'ndk;20.0.5594570'
        echo y | sdkmanager 'cmake;3.10.2.4988404'
    fi;

    git submodule update -q --init --recursive
}

travis_script()
{
    if [ "$TARGET_OS" = "Android" ]; then
        pushd build_android
        ./gradlew
        ./gradlew assembleRelease
        popd 
    elif [ "$TARGET_OS" = "Linux_Clang_Format" ]; then
        set +e
        find ./Source/ ./tools/ -iname *.h -o -iname *.cpp -o -iname *.m  -iname *.mm | xargs clang-format-6.0 -i
        git config --global user.name "Clang-Format"
        git config --global user.email "Clang-Format"
        git commit -am"Clang-format";
        if [ $? -eq 0 ]; then
            url=$(git format-patch -1 HEAD --stdout | nc termbin.com 9999)
            echo "generated clang-format patch can be found at: $url"
            echo "you can pipe patch directly using the following command:";
            echo "curl $url | git apply -v"
            echo "then manually commit and push the changes"
            exit -1;
        fi
        exit 0;
    else
        mkdir build
        pushd build
        
        if [ "$TARGET_OS" = "Linux" ]; then
            if [ "$CXX" = "g++" ]; then export CXX="g++-9" CC="gcc-9"; fi
            source /opt/qt512/bin/qt512-env.sh || true
            export PATH=$PATH:/opt/qt512/lib/cmake
            cmake .. -G"$BUILD_TYPE" -DCMAKE_INSTALL_PREFIX=./appdir/usr;
            cmake --build .
            ctest
            cmake --build . --target install
            if [ "$TARGET_ARCH" = "x86_64" ]; then
                mkdir -p appdir/usr/optional/
                mkdir -p appdir/usr/optional/libstdc++/
                cp /usr/lib/*-linux-gnu/libstdc++.so.6 ./appdir/usr/optional/libstdc++/
                cp ../AppRun ./appdir/AppRun
                cp ../exec.so ./appdir/usr/optional/exec.so
                printf "#include <memory>\nint main(){std::make_exception_ptr(0);}" | $CXX -x c++ -o ./appdir/usr/optional/checker -

                mkdir -p appdir/usr/share/doc/libc6/
                echo "" > appdir/usr/share/doc/libc6/copyright
                # AppImage Creation
                unset QTDIR; unset QT_PLUGIN_PATH; unset LD_LIBRARY_PATH;
                export VERSION="${TRAVIS_COMMIT:0:8}"
                ../linuxdeployqt*.AppImage ./appdir/usr/share/applications/*.desktop -bundle-non-qt-libs -unsupported-allow-new-glibc -qmake=`which qmake`
                ../linuxdeployqt*.AppImage ./appdir/usr/share/applications/*.desktop -appimage -unsupported-allow-new-glibc -qmake=`which qmake`
            fi
        elif [ "$TARGET_OS" = "OSX" ]; then
            export CMAKE_PREFIX_PATH="$(brew --prefix qt5)"
            export VULKAN_SDK=$(pwd)/../vulkansdk-macos-${VULKAN_SDK_VERSION}/macOS
            cmake .. -G"$BUILD_TYPE"
            cmake --build . --config Release
            ctest -C Release
            $(brew --prefix qt5)/bin/macdeployqt Source/ui_qt/Release/Play.app
            appdmg ../installer_macosx/spec.json Play.dmg
        elif [ "$TARGET_OS" = "IOS" ]; then
            cmake .. -G"$BUILD_TYPE" -DCMAKE_TOOLCHAIN_FILE=../deps/Dependencies/cmake-ios/ios.cmake -DTARGET_IOS=ON -DBUILD_PSFPLAYER=ON
            cmake --build . --config Release
            codesign -s "-" Source/ui_ios/Release-iphoneos/Play.app
            pushd ..
            pushd installer_ios
            ./build_cydia.sh
            ./build_ipa.sh
            popd
            popd
        fi;
        
        popd
    fi;
}

travis_before_deploy()
{
    export SHORT_HASH="${TRAVIS_COMMIT:0:8}"
    mkdir deploy
    pushd deploy
    mkdir $SHORT_HASH
    pushd $SHORT_HASH
    if [ -z "$ANDROID_KEYSTORE_PASS" ]; then
        return
    fi;
    if [ "$TARGET_OS" = "Linux" ]; then
        if [ "$TARGET_ARCH" = "x86_64" ]; then
            cp ../../build/Play*.AppImage .
        fi;
    fi;
    if [ "$TARGET_OS" = "Android" ]; then
        cp ../../build_android/build/outputs/apk/release/Play-release-unsigned.apk .
        export ANDROID_BUILD_TOOLS=$ANDROID_HOME/build-tools/28.0.3
        $ANDROID_BUILD_TOOLS/zipalign -v -p 4 Play-release-unsigned.apk Play-release.apk
        $ANDROID_BUILD_TOOLS/apksigner sign --ks ../../installer_android/deploy.keystore --ks-key-alias deploy --ks-pass env:ANDROID_KEYSTORE_PASS --key-pass env:ANDROID_KEYSTORE_PASS Play-release.apk
    fi;
    if [ "$TARGET_OS" = "OSX" ]; then
        cp ../../build/Play.dmg .
    fi;
    if [ "$TARGET_OS" = "IOS" ]; then
        cp ../../installer_ios/Play.ipa .
        cp ../../installer_ios/Play.deb .
        cp ../../installer_ios/Packages.bz2 .
    fi;
    popd
    popd
}

set -e
set -x

$1;
