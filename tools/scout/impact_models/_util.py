r"""the shared mutation switch of the impact models (tools\scout\impact.py --mutate turns every model's own
mutation control on at once; each must then fail its own check and nothing else)"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import pairs as P  # noqa: E402

MUTATE = P.MUTATE          # the same dict impact.py and pairs.py switch


class _D:                  # the MUT = {'on': ...} form
    def __getitem__(self, k):
        return bool(MUTATE["on"])


class _X:                  # the MUT = False form (used as a bool)
    def __bool__(self):
        return bool(MUTATE["on"])


class _N(dict):            # the named form: MUT['on'] holds a mutation name
    def __getitem__(self, k):
        return "on" if MUTATE["on"] else None


MUTD, MUTX, MUTN = _D(), _X(), _N()
