"""External libraries used by the receiver's explicit FFmpeg recipes."""
from pathlib import Path


def hardware_libraries(prefix, windows):
    if windows:
        return []
    metadata = Path(prefix) / 'lib/pkgconfig/libavutil.pc'
    if not metadata.is_file():
        raise RuntimeError('Missing FFmpeg link metadata: ' + str(metadata))
    # The fixed VAAPI recipe disables X11 and links libva through DRM. Keep
    # this based on the selected prefix, including --ffmpeg-root replacements.
    if '-lva' in metadata.read_text(encoding='utf-8'):
        return ['-lva-drm', '-lva', '-ldrm']
    return []
