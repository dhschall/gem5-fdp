from string import Template

import pathlib
x = str(pathlib.Path(__file__).parent.resolve())

with open(x+"/x86-ubuntu-run-with-kvm-uipi-accel.py.template", "r") as f:
    src = Template(f.read())

import argparse

parser = argparse.ArgumentParser(description='Create gem5fs script')
parser.add_argument('--int', dest='interrupt_type',type=str,choices=["N","Flush","Drain","Track","Apic"])
parser.add_argument('--e', dest='max_error', type=float)
parser.add_argument('--len', dest='request_length', type=int,default=0)
parser.add_argument('--pci', dest='is_pci', type=int, default=1)
parser.add_argument('--vmr', dest='vmr', type=float, default=1)

args = parser.parse_args()
interrupt_type = str(args.interrupt_type)
max_error = float(str(args.max_error))
request_length = int(str(args.request_length))
if interrupt_type == 'N':
    interrupt_type = ''
is_pci = int(str(args.is_pci)) 
vmr = float(str(args.vmr))
# if interrupt_type == '':
#     assert(test_type == 'ncm' or test_type == 'nch')
# else:
#     assert(test_type == 'cm' or test_type == 'ch')
res = src.substitute(
    {
        "interrupt_type": interrupt_type,
        "max_error":max_error,
        "request_length":request_length,
        "is_pci":is_pci,
        "vmr": vmr,
    }
)
with open(x+"/x86-ubuntu-run-with-kvm-uipi-accel.py", "w") as f:
    f.write(res)