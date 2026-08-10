# External Source Trees

`External/xnu` is the upstream Apple XNU source tree used by the XNU
integration checks. It is intentionally not tracked in this repository.

Project-owned integration code must live outside `External/xnu`; CI fetches the
tree from Apple and makes it read-only before running the checks.
