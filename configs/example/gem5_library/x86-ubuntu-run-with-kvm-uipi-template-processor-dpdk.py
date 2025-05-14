from string import Template

import pathlib
x = str(pathlib.Path(__file__).parent.resolve())

with open(x+"/x86-ubuntu-run-with-kvm-uipi-dpdk.py.template", "r") as f:
    src = Template(f.read())

import argparse

parser = argparse.ArgumentParser(description='Create gem5fs script')
parser.add_argument('--int', dest='interrupt_type',type=str,choices=["N","Flush","Drain","Track","Apic"])
parser.add_argument('--p', dest='packet_rate', type=float)
parser.add_argument('--interval', dest='interrupt_interval', type=int,default=0)
parser.add_argument('--pci', dest='is_pci', type=int, default=1)
parser.add_argument('--alt', dest='alteration_rate', type=int, default=10)
parser.add_argument('--m', dest='mode', type=int, default=1)

args = parser.parse_args()
interrupt_type = str(args.interrupt_type)
packet_rate = float(str(args.packet_rate))
interrupt_interval = int(str(args.interrupt_interval))
if interrupt_type == 'N':
    interrupt_type = ''
is_pci = int(str(args.is_pci)) 
alteration_rate = int(str(args.alteration_rate))
mode = int(str(args.mode))
# if interrupt_type == '':
#     assert(test_type == 'ncm' or test_type == 'nch')
# else:
#     assert(test_type == 'cm' or test_type == 'ch')
res = src.substitute(
    {
        "interrupt_type": interrupt_type,
        "packet_rate":packet_rate,
        "interrupt_interval":interrupt_interval,
        "is_pci":is_pci,
        "alteration_rate": alteration_rate,
        "mode": mode,
    }
)
with open(x+"/x86-ubuntu-run-with-kvm-uipi-dpdk.py", "w") as f:
    f.write(res)