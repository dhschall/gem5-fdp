#ifndef __RECORDER_PROBE_HH__
#define __RECORDER_PROBE_HH__

#include <sim/probe/probe.hh>

namespace gem5
{

template <class T, class Arg>
class RecorderListenerBaseArg : public ProbeListenerArgBase<Arg>
{
private:
    T *const object;
    void (T::* function)(const Arg &);

public:
    RecorderListenerBaseArg(
        T *obj, ProbeManager *target,
        const std::string &name,
        void (T::* func)(const Arg &)
    )
        : ProbeListenerArgBase<Arg>(target, name),
        object(obj),
        function(func)
    {}

    void notify(const Arg &val) override { (object->*function)(val); }
};

} // gem5

#endif
