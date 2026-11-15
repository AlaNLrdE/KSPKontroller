Maintaining the documentation
=============================

Documentation structure
-----------------------

The site is authored in reStructuredText and built by Sphinx. Architecture diagrams are editable
PlantUML sources in ``docs/diagrams/*.puml`` included with ``.. uml::`` directives. Doxygen generates
the source API reference for the C# mod, Arduino sketch and C++ HMI; Sphinx is the main navigation
and explanatory documentation, not a generated API dump.

.. list-table:: Where a change belongs
   :header-rows: 1
   :widths: 35 65

   * - Topic
     - Source
   * - Product boundary and deployment
     - ``overview.rst``, ``architecture.rst``, deployment/component PUML files.
   * - Protocol/channel contracts
     - ``communication.rst``, ``protocol.rst``, ``vesseldata.rst`` and sequence/state diagrams.
   * - Per-project implementation
     - ``development/arduino.rst``, ``development/plugin.rst``, ``development/raspberry-pi.rst``.
   * - Usage/configuration and recovery
     - ``firmware.rst``, ``plugin.rst``, ``hmi.rst``, ``simulator.rst``, ``debugging.rst``.
   * - Build, verification and coverage
     - ``building.rst``, ``testing.rst``, ``ci`` and ``tools/verify.py``.
   * - API types, functions and members
     - Source comments plus ``Doxyfile`` input configuration.

Build locally
-------------

Install Python 3, a working Java runtime and Graphviz; Doxygen is needed for a complete API reference.
``./ci sphinx`` creates a Python virtual environment, installs ``docs/requirements.txt`` and downloads
PlantUML into ``build/tools`` when absent. Initial dependency setup needs network access.

.. code-block:: bash

   python3 tools/verify.py --docs-only
   ./ci diagrams
   ./ci docs
   ./ci sphinx

Open ``build/sphinx/index.html``. Rendered standalone UML is under ``build/diagrams``. The API is
generated into ``build/docs/html`` and copied to ``build/sphinx/api``. Without Doxygen, Sphinx still
builds the narrative site but the API link has no generated destination.

``PLANTUML_JAR`` overrides the downloaded JAR. On macOS a Homebrew JDK may need its ``bin`` directory
on ``PATH`` (or ``JAVA_HOME`` pointing at that JDK); an existing ``/usr/bin/java`` can be only a stub.
The build checks that Java can actually run. Keep paths quoted so a repository/JAR path can contain spaces.

Sphinx runs in nitpicky mode, with warnings treated as errors. Fix unresolved cross-references,
missing diagrams, heading errors and unused documents instead of suppressing them globally. Render
all PUML files as well: an unused source should not silently contain a syntax error.

Diagram conventions
-------------------

Use component/deployment diagrams for ownership and processes, class diagrams for real interfaces,
sequence diagrams for message ordering, and state/activity diagrams for transitions and scheduling.
Label network arrows with transport, ports and direction. Show source/worker/render thread boundaries
when ordering matters. Every sequence diagram declares its participants/lifelines and explicitly shows
activation bars for their illustrated work, returns and asynchronous messages. Dashed vertical lifelines
show a participant's presence; activation bars show an execution interval, not CPU utilization while
waiting. Explain conceptual roles that are not actual classes, and distinguish worker lifetime from a
single method's activation. Keep activation/deactivation balanced for every declared participant.

Keep a single ``@startuml`` / ``@enduml`` pair per file. Use relative ``.. uml::`` paths from the
including RST page. Use ``!pragma layout smetana`` for supported graph layouts when appropriate;
Graphviz remains available for diagrams requiring it. Commit the PUML sources, not generated SVG/HTML.
Do not use Mermaid in the published UML documentation.

Accuracy and maintenance
------------------------

Trace each claim to code, configuration or a recorded test. Document desired future behavior as a
proposal, never as an implemented capability. Include default values, units, disabled controls,
unavailable-data behavior and failure/recovery semantics. Keep packet offsets in the field reference
rather than maintaining competing copies in every project guide.

When a protocol or lifecycle changes, update the relevant guide and diagrams in the same change.
Keep internal source paths relative to the repository; do not publish a personal Pi address, login,
token or machine-specific filesystem path. Write documentation and source comments in English.

The documentation workflow builds on changes to narrative docs, UML, API sources and build configuration.
GitHub Pages publication follows pushes to ``main`` through ``.github/workflows/docs.yml``; do not push
without authorization for that push. A local successful build is not proof that Pages has published.
