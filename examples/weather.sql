CREATE TABLE weather (
  id INT KEY,
  city TEXT INDEX,
  country TEXT INDEX,
  temperature INT,
  humidity INT,
  conditions TEXT INDEX
);
