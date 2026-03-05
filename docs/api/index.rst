REST API Reference
##################

The PMWEBAPI (Performance Metrics Web Application Programming Interface) is a collection of
interfaces providing Performance Co-Pilot services for web applications. It consists of APIs
for web applications querying and analysing both live and historical performance data, as well
as APIs used by web servers.

The usual HTTP URL-encoded optional parameter rules apply and PMWEBAPI REST requests always
follow the convention::

    /api/endpoint?parameter1=value1&parameter2=value2

Examples in all following sections use the ``curl``\(1) command line utility with a local
``pmproxy``\(1) server listening on port 44322 (default port).

.. openapi:: ../specs/openapi.yaml
   :format: markdown
