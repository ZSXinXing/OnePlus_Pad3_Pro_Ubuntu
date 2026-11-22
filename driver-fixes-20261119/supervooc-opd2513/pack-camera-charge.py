#!/usr/bin/env python3
"""Pack a RAM-only charge test boot while retaining the tested camera baseline."""
import argparse
import ctypes as C
import gzip
import pathlib, shutil, tempfile, json, struct, subprocess, hashlib
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2] / 'ram-rehearsal/camera-control-20261121')
parser.add_argument('--battery-module', type=pathlib.Path, required=True)
parser.add_argument('--output-dir', type=pathlib.Path, required=True)
parser.add_argument('--vooc-module', type=pathlib.Path)
parser.add_argument('--adc-module-dir', type=pathlib.Path)
parser.add_argument('--oem-normal-high', action='store_true', help='enable the guarded normal-high trial')
parser.add_argument('--svooc-skip-soc-step', action='store_true',
                    help='enable the tested 6.1 A normal-low profile with the connector fix')
args = parser.parse_args()
if bool(args.vooc_module) != bool(args.adc_module_dir):
    parser.error('--vooc-module and --adc-module-dir must be used together')
if args.oem_normal_high and not args.vooc_module:
    parser.error('--oem-normal-high requires the SuperVOOC module')
if args.svooc_skip_soc_step and not (args.vooc_module and args.oem_normal_high):
    parser.error('--svooc-skip-soc-step requires --vooc-module and --oem-normal-high')
src = args.base.resolve()
base = src
out = args.output_dir.resolve()
out.mkdir(parents=True, exist_ok=False)
manifest = json.loads((base/'manifest.json').read_text())
for name in ['Image','payload.S']:
    shutil.copy2(src/name, out/name)
assert hashlib.sha256((out/'Image').read_bytes()).hexdigest() == manifest['kernel_sha256']
archive = gzip.decompress((src/'initrd.gz').read_bytes())
assert hashlib.sha256((src/'initrd.gz').read_bytes()).hexdigest() == manifest['initrd_sha256']
module = args.battery_module.read_bytes()
assert module.startswith(b'\x7fELF'), 'battery module must be an ELF kernel module'
extra_modules = {}
if args.vooc_module:
    extra_modules = {
        b'qcom-vadc-common.ko': (args.adc_module_dir / 'qcom-vadc-common.ko').read_bytes(),
        b'qcom-spmi-adc5-gen3.ko': (args.adc_module_dir / 'qcom-spmi-adc5-gen3.ko').read_bytes(),
        b'opd2513_vooc.ko': args.vooc_module.read_bytes(),
    }
    assert all(data.startswith(b'\x7fELF') for data in extra_modules.values())


def archive_file(header, name, data):
    name += b'\0'
    header = (header[:54] + f'{len(data):08x}'.encode() + header[62:94] +
              f'{len(name):08x}'.encode() + header[102:])
    entry = header + name
    return entry + bytes(-len(entry) % 4) + data + bytes(-len(data) % 4)


load_vooc = b"""# Load the verified charging modules for this temporary Ubuntu boot.
if $bb tr '\\000' '\\n' </sys/firmware/devicetree/base/compatible | $bb grep -qx oneplus,opd2513; then
 if $bb insmod /qcom-vadc-common.ko &&
    $bb insmod /qcom-spmi-adc5-gen3.ko &&
    $bb insmod /opd2513_vooc.ko powered_trial=1 dual_trial=1 oem_policy=1 oem_first_3a=1 oem_high_band_first_6a1=1; then
  printf 'OPD2513_SUPERVOOC_MODULES_LOADED\\n'
 else
  printf 'OPD2513_SUPERVOOC_MODULE_LOAD_FAILED\\n'
 fi
fi
"""
if args.oem_normal_high:
    load_vooc = load_vooc.replace(b'oem_high_band_first_6a1=1;',
                                  b'oem_high_band_first_6a1=1 oem_normal_high=1;')
if args.svooc_skip_soc_step:
    load_vooc = load_vooc.replace(b'oem_normal_high=1;',
                                  b'oem_normal_high=1 svooc_skip_soc_step=1;')
updated = bytearray()
module_header = None
init_count = 0
pos = count = 0
while pos + 110 <= len(archive):
    header = archive[pos:pos+110]
    assert header[:6] == b'070701'
    size = int(header[54:62], 16)
    name_size = int(header[94:102], 16)
    start = (pos + 110 + name_size + 3) & ~3
    end = (start + size + 3) & ~3
    name = archive[pos+110:pos+110+name_size-1]
    if name == b'qcom_battmgr.ko':
        module_header = header
        updated.extend(header[:54] + f'{len(module):08x}'.encode() + header[62:] + archive[pos+110:start])
        updated.extend(module + bytes(-len(module) % 4))
        count += 1
    elif name == b'init' and extra_modules:
        init = archive[start:start + size]
        anchor = b'$bb mount --move /run /newroot/run\n'
        assert init.count(anchor) == 1
        updated.extend(archive_file(header, name, init.replace(anchor, load_vooc + anchor)))
        init_count += 1
    else:
        assert name not in extra_modules, name
        if name == b'TRAILER!!!' and extra_modules:
            assert module_header is not None and init_count == 1
            for module_name, data in extra_modules.items():
                updated.extend(archive_file(module_header, module_name, data))
        updated.extend(archive[pos:end])
    pos = end
    if name == b'TRAILER!!!':
        break
assert count == 1 and name == b'TRAILER!!!'
(out/'initrd.gz').write_bytes(gzip.compress(updated + archive[pos:], mtime=0))
link = manifest['commands'][-1]
for name in link:
    if name.endswith('.obj'):
        shutil.copy2(src/name, out/name)
fdt = C.CDLL('libfdt.so.1')
fdt.fdt_getprop.restype = C.c_void_p
fdt.fdt_getprop.argtypes = [C.c_void_p,C.c_int,C.c_char_p,C.POINTER(C.c_int)]
raw = (src/'native.dtb').read_bytes()
buf = C.create_string_buffer(len(raw)+8192)
assert fdt.fdt_open_into(raw,buf,len(buf)) == 0
def node_at(path):
    node = fdt.fdt_path_offset(buf,path)
    assert node >= 0, (path,node)
    return node

def set_prop(node,name,value):
    assert fdt.fdt_setprop(buf,node,name,value,len(value)) == 0, name

def cells(*v):
    return struct.pack('>'+'I'*len(v),*v)

pin_path = b'/soc@0/pinctrl@f100000'
bus_path = b'/soc@0/geniqup@9c0000/i2c@980000'
tlmm = fdt.fdt_get_phandle(buf,node_at(pin_path))
assert tlmm > 0
maxph = C.c_uint32()
assert fdt.fdt_find_max_phandle(buf,C.byref(maxph)) == 0
irq_pin = maxph.value + 1
pin = fdt.fdt_add_subnode(buf,node_at(pin_path),b'vooc-irq-state')
assert pin >= 0
for name,value in ((b'phandle',cells(irq_pin)),(b'pins',b'gpio0\0'),(b'function',b'gpio\0'),(b'input-enable',b''),(b'drive-strength',cells(2)),(b'bias-pull-up',b'')):
    set_prop(pin,name,value)
charger = fdt.fdt_add_subnode(buf,node_at(bus_path),b'charger@6a')
assert charger >= 0
for name,value in ((b'compatible',b'southchip,sc8547d\0'),(b'reg',cells(0x6a)),(b'interrupts-extended',cells(tlmm,0,2)),(b'pinctrl-names',b'default\0'),(b'pinctrl-0',cells(irq_pin))):
    set_prop(charger,name,value)
adc_path = b'/soc@0/arbiter@c400000/spmi@c426000/pmic@0/adc@9000'
adc_ph = maxph.value + 2
if old_ph := fdt.fdt_get_phandle(buf,node_at(adc_path)):
    adc_ph = old_ph
else:
    set_prop(node_at(adc_path),b'phandle',cells(adc_ph))
for reg,label in ((0x2744,b'batt_con_therm_adc'),(0x2747,b'usb_con_therm_adc'),(0x274b,b'svooc_mos_btb_therm')):
    node = fdt.fdt_add_subnode(buf,node_at(adc_path),f'channel@{reg:x}'.encode())
    assert node >= 0
    for name,value in ((b'reg',cells(reg)),(b'label',label+b'\0'),(b'qcom,ratiometric',b''),(b'qcom,hw-settle-time',cells(200))):
        set_prop(node,name,value)
charger = node_at(bus_path+b'/charger@6a')
set_prop(charger,b'io-channels',cells(adc_ph,0x2747,adc_ph,0x2744,adc_ph,0x274b))
set_prop(charger,b'io-channel-names',b'usb-connector\0battery-connector\0vooc-mos\0')
for path in (bus_path, b'/soc@0/geniqup@8c0000/i2c@890000'):
    set_prop(node_at(path),b'status',b'okay\0')
    set_prop(node_at(path),b'clock-frequency',cells(400000))
set_prop(node_at(b'/soc@0/geniqup@9c0000'),b'status',b'okay\0')
assert fdt.fdt_pack(buf) == 0
# Abort if charging changes removed the tested camera, flash or bus aliases.
checks = {}
def prop(path, name):
    size = C.c_int()
    value = fdt.fdt_getprop(buf, node_at(path), name, C.byref(size))
    return C.string_at(value, size.value) if value else None
for path in (b'/soc@0/isp@9253000',
             b'/soc@0/arbiter@c400000/spmi@c426000/pmic@1/led-controller@ee00',
             bus_path, b'/soc@0/geniqup@8c0000/i2c@890000'):
    assert prop(path, b'status') in (b'okay\0', b'ok\0'), path
    checks[path.decode()] = 'okay'
for number in (0, 1, 2, 3, 4, 9):
    name = f'i2c{number}'.encode()
    value = prop(b'/aliases', name)
    assert value and node_at(value.rstrip(b'\0')) >= 0, name
    checks[name.decode()] = value.rstrip(b'\0').decode()
(out/'native.dtb').write_bytes(buf.raw[:struct.unpack_from('>I',buf.raw,4)[0]])
subprocess.run(next(cmd for cmd in manifest['commands'] if cmd[-1]=='payload.obj'),cwd=out,check=True)
subprocess.run(link,cwd=out,check=True)
loader=(out/'loader.efi').read_bytes()
with (src/'boot.img').open('rb') as f:
    header=bytearray(f.read(4096))
struct.pack_into('<I',header,8,len(loader))
image=bytes(header)+loader+bytes(-len(loader)%4096)
(out/'boot.img').write_bytes(image)
record = {
    'scope': 'fastboot boot only; no partition writes',
    'oem_normal_high': args.oem_normal_high,
    'svooc_skip_soc_step': args.svooc_skip_soc_step,
    'base': str(src),
    'battery_module': str(args.battery_module.resolve()),
    'battery_module_sha256': hashlib.sha256(module).hexdigest(),
    'base_initrd_sha256': manifest['initrd_sha256'],
    'cpio_replaced_entries': ['qcom_battmgr.ko'] + (['init'] if extra_modules else []),
    'cpio_added_module_sha256': {name.decode(): hashlib.sha256(data).hexdigest()
                                 for name, data in extra_modules.items()},
    'device_tree_checks': checks,
    'sha256': {name: hashlib.sha256((out/name).read_bytes()).hexdigest()
               for name in ('boot.img', 'Image', 'native.dtb', 'initrd.gz')},
}
(out/'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
print(out, record['sha256']['boot.img'])
