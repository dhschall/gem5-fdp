from string import Template

import pathlib
x = str(pathlib.Path(__file__).parent.resolve())

with open(x+"/x86-ubuntu-run-with-kvm-uipi-chase.py.template", "r") as f:
    src = Template(f.read())

import argparse

parser = argparse.ArgumentParser(description='Create gem5fs script')
parser.add_argument('--int', dest='interrupt_type',type=str,choices=["N","Flush","Drain","Track"])
parser.add_argument('--td', dest='time_delta', type=int,default=0)
parser.add_argument('--si', dest='size_in', type=int, default=18)

args = parser.parse_args()
interrupt_type = str(args.interrupt_type)
size_in = str(args.size_in)
time_delta = str(args.time_delta)
if interrupt_type == 'N':
    interrupt_type = ''
# if interrupt_type == '':
#     assert(test_type == 'ncm' or test_type == 'nch')
# else:
#     assert(test_type == 'cm' or test_type == 'ch')
res = src.substitute(
    {
        "interrupt_type": interrupt_type,
        "size_in" : size_in,
        "time_delta": time_delta,
    }
)
with open(x+"/x86-ubuntu-run-with-kvm-uipi-chase.py", "w") as f:
    f.write(res)