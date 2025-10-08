#########################################################################
# Your <SDK_ROOT_PATH> folder structure should look like this:
# .
# ├── host-tools
# ├── cvitek-tdl-sdk-sg200x
#########################################################################

export CVITEK_SDK_PATH=<SDK_ROOT_PATH>
export SDK_ROOT_PATH=$CVITEK_SDK_PATH/cvitek-tdl-sdk-sg200x
export PATH=$PATH:$CVITEK_SDK_PATH/host-tools/gcc/riscv64-linux-musl-x86_64/bin
export KERNEL_ROOT=$SDK_ROOT_PATH/sample
export MW_PATH=$SDK_ROOT_PATH/sample/3rd/middleware/v2
export TPU_PATH=$SDK_ROOT_PATH/sample/3rd/tpu
export IVE_PATH=$SDK_ROOT_PATH/sample/3rd/ive
export USE_TPU_IVE=OFF
export CHIP=CV181X
export SDK_VER=musl_riscv64
