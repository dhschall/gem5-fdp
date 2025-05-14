from string import Template

import pathlib
x = str(pathlib.Path(__file__).parent.resolve())

with open(x+"/x86-ubuntu-run-with-kvm-uipi-spec.py.template", "r") as f:
    src = Template(f.read())

import argparse

parser = argparse.ArgumentParser(description='Create gem5fs script')
parser.add_argument('--int', dest='interrupt_type',type=str,choices=["N","Flush","Drain","Track","Apic"])
parser.add_argument('--interval', dest='interrupt_interval', type=int,default=0)
parser.add_argument('--t', dest='is_hard_timer',type=int, default=1)
parser.add_argument('--test', dest='test_name', type=str)

args = parser.parse_args()
interrupt_type = str(args.interrupt_type)
interrupt_interval = int(str(args.interrupt_interval))
is_hard_timer = int(str(args.is_hard_timer))
test_name = str(args.test_name)

if interrupt_type == 'N':
    interrupt_type = ''
# if interrupt_type == '':
#     assert(test_type == 'ncm' or test_type == 'nch')
# else:
#     assert(test_type == 'cm' or test_type == 'ch')
res = src.substitute(
    {
        "interrupt_type": interrupt_type,
        "interrupt_interval":interrupt_interval,
        "is_hard_timer":is_hard_timer,
        "test_name":test_name,
    }
)
with open(x+"/x86-ubuntu-run-with-kvm-uipi-spec.py", "w") as f:
    f.write(res)
