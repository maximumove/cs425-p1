# Project 1

- Name: Troy Berhow
- Email: troyberhow@u.boisestate.edu
- Class: CS525-001

## Known Bugs or Issues

There are not any know issues or bugs.

## Experience

This project was really daunting to get started, since it featured something I had never really dabbled in before. I also did not catch that there was a sperate document for SMTP, so I was initally heistant to try. While I devloped I had claude bring up relevant info from the document when I needed it, which helped me quite a lot. Layer 2 was by far the hardest part, with all the cases in smtp_run_session being a pain to go back and add. Once again outside research and claude helped me to find the information when I needed to find it.

As for the actual project it was really quite fascinating. Sending mail to a server sounds mundane at first, but it was really cool to figure out how it was done and how it could maybe be expanded on. When I got the project to send a message to the server I felt so satisfied.

## Design

The client is split into three layers: pure protocol helpers that build and parse strings with no I/O, a session layer that drives the SMTP exchange through a read/write callback pair (a swappable "transport"), and a thin socket transport that implements those callbacks over a real TCP connection. This split lets the session logic be fully unit tested against a scripted in-memory transport instead of a live mail server.