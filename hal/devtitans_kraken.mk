# Herda as configurações do emulador (produto sdk_phone_x86_64)
$(call inherit-product, $(SRC_TARGET_DIR)/product/sdk_phone_x86_64.mk)

# Device Framework Matrix (Declara que o nosso produto Kraken precisa do serviço smartlamp)
DEVICE_FRAMEWORK_COMPATIBILITY_MATRIX_FILE := device/devtitans/kraken/device_framework_matrix.xml

# Sobrescreve algumas variáveis com os dados do novo produto
PRODUCT_NAME := devtitans_kraken
PRODUCT_DEVICE := kraken
PRODUCT_BRAND := KrakenBrand
PRODUCT_MODEL := KrakenModel

# Copia o arquivo devtitans.txt para o /system/etc da imagem do Android
PRODUCT_COPY_FILES += \
    device/devtitans/kraken/devtitans.txt:system/etc/devtitans.txt \
    device/devtitans/kraken/kraken.rc:vendor/etc/init/kraken.rc \
    device/devtitans/kraken/bootanimation.zip:product/media/bootanimation.zip \
    device/devtitans/kraken/prebuilt/wallpaper/default_wallpaper2.png:product/media/default_wallpaper2.png

# device/<vendor>/<produto>/<teu>.mk
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.consumerir.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.consumerir.xml

PRODUCT_SYSTEM_PROPERTIES += \
    ro.devtitans.name=Kraken

PRODUCT_PRODUCT_PROPERTIES += \
    ro.product.devtitans.version=1.0 \
    ro.config.wallpaper=/product/media/default_wallpaper2.png

PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.devtitans.hardware=ModelB

# Seta o diretório de overlays
PRODUCT_PACKAGE_OVERLAYS = device/devtitans/kraken/overlay

PRODUCT_PACKAGES += \
    UniversalMediaPlayer \
    hello_c \
    nano \
    sl \
    hello_cpp \
    hello_cpp_lib \
    hello_daemon_cpp \
    HelloApp \
    HelloJava \
    smartlamp_client \
    devtitans.smartlamp \
    devtitans.smartlamp-service \
    smartlamp_service_client \
    SmartlampTestApp \
    devtitans.smartlampmanager \
    android.hardware.ir-service.example

BOARD_SEPOLICY_DIRS += device/devtitans/kraken/sepolicy